#include "filemanager/folder_size.h"
#include "filemanager/property_operations.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <gio/gio.h>
#include <memory>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
  filemanager::FolderSizeRequest request(const std::filesystem::path& path) {
    struct stat state{};
    assert(lstat(path.c_str(), &state) == 0);
    return {path, static_cast<std::uint64_t>(state.st_dev), state.st_ino};
  }
  void write(const std::filesystem::path& path, std::size_t bytes) { std::ofstream(path) << std::string(bytes, 'x'); }
} // namespace
int main() {
  assert(geteuid() != 0);
  char* raw = g_dir_make_tmp("nythera-folder-size-XXXXXX", nullptr);
  assert(raw);
  const std::filesystem::path fixture(raw);
  g_free(raw);
  const auto root = fixture / "tree";
  std::filesystem::create_directories(root / "nested");
  write(root / "one", 5);
  write(root / "nested/two", 7);
  write(fixture / "outside", 100);
  std::filesystem::create_hard_link(root / "one", root / "same-data");
  const std::string outside = "../outside", broken = "missing", directory = "nested";
  std::filesystem::create_symlink(outside, root / "outside-link");
  std::filesystem::create_symlink(broken, root / "broken-link");
  std::filesystem::create_directory_symlink(directory, root / "directory-link");
  const auto target = request(root);
  const auto count = [](const std::filesystem::path& path, GCancellable* cancellable = nullptr) {
    g_autoptr(GFile) file = g_file_new_for_path(path.c_str());
    return filemanager::countImmediateFolderItems(file, cancellable);
  };
  const auto empty = fixture / "empty";
  std::filesystem::create_directory(empty);
  assert(count(empty) == 0);
  assert(count(root / "nested") == 1);
  assert(count(root) == 6);
  assert(!count(fixture / "missing") && !count(root / "one"));
  g_autoptr(GCancellable) stoppedCount = g_cancellable_new();
  g_cancellable_cancel(stoppedCount);
  assert(!count(root, stoppedCount));
  const auto expected = 17 + outside.size() + broken.size() + directory.size();
  auto result = filemanager::calculateFolderSize(target);
  assert(
      !result.partial() && result.bytes == expected && result.files == 3 && result.folders == 1 && result.links == 3
  );
  // The root directory inode size and the outside referent's contents are excluded.
  assert(result.bytes < 100);
  auto boundary = filemanager::calculateFolderSize(target, {}, {.openChild = [](int fd, const char* path) {
                                                     if (std::string_view(path) == "nested") {
                                                       errno = EXDEV;
                                                       return -1;
                                                     }
                                                     return filemanager::openPropertyDescendant(fd, path);
                                                   }});
  assert(boundary.partial() && boundary.excludedMounts == 1 && boundary.bytes == expected - 7 && boundary.folders == 0);
  assert(chmod((root / "nested").c_str(), 0000) == 0);
  assert(!count(root / "nested") && count(root) == 6);
  auto denied = filemanager::calculateFolderSize(target);
  assert(denied.partial() && denied.errors == 1 && denied.bytes == expected - 7);
  // The unreadable directory entry itself is counted; its descendants are not.
  assert(denied.folders == 1 && denied.files == 2 && denied.links == 3);
  assert(denied.diagnostics.front().path == "nested");
  assert(denied.diagnostics.front().operation == "open directory" && denied.diagnostics.front().subtree);
  assert(denied.diagnostics.front().error == EACCES && !denied.diagnostics.front().description.empty());
  assert(std::string_view(filemanager::folderSizeStatus(denied)) == "partial");
  assert(chmod((root / "nested").c_str(), 0700) == 0);
  auto depth = filemanager::calculateFolderSize(target, {}, {.maxDepth = 1});
  assert(depth.partial() && depth.bytes == expected - 7);
  auto disappearing = filemanager::calculateFolderSize(target, {}, {.openChild = [&](int fd, const char* path) {
                                                         if (std::string_view(path) == "nested/two")
                                                           std::filesystem::remove(root / path);
                                                         return filemanager::openPropertyDescendant(fd, path);
                                                       }});
  assert(disappearing.partial() && disappearing.bytes == expected - 7);
  write(root / "nested/two", 7);
  int checks = 0;
  auto cancelled = filemanager::calculateFolderSize(target, [&] { return ++checks > 4; });
  assert(cancelled.cancelled && cancelled.partial() && cancelled.bytes < expected);
  auto immediately = filemanager::calculateFolderSize(target, [] { return true; });
  assert(immediately.cancelled && immediately.bytes == 0);
  assert(std::string_view(filemanager::folderSizeStatus(immediately)) == "cancelled");
  assert(std::string_view(filemanager::folderSizeStatus(result)) == "complete");
  // Hidden names participate; the root is excluded and links remain separate.
  const auto hiddenTree = fixture / "hidden-tree";
  std::filesystem::create_directories(hiddenTree / ".hidden-dir");
  write(hiddenTree / ".hidden-file", 3);
  write(hiddenTree / ".hidden-dir/child", 4);
  assert(count(hiddenTree) == 2);
  const auto hiddenResult = filemanager::calculateFolderSize(request(hiddenTree));
  assert(!hiddenResult.partial() && hiddenResult.files == 2 && hiddenResult.folders == 1 && hiddenResult.bytes == 7);
  const auto limited = filemanager::calculateFolderSize(request(hiddenTree), {}, {.maxEntries = 1});
  assert(limited.partial() && !limited.failed && limited.reason.find("entry limit") != std::string::npos);
  // One failure can exclude a whole subtree. NFS stale handles and permission
  // failures retain their distinct errno and relative path without real NFS I/O.
  for (const int error : {EACCES, ESTALE, EIO, ENOENT}) {
    const auto omitted = filemanager::calculateFolderSize(target, {}, {.openChild = [error](int fd, const char* path) {
                                                            if (std::string_view(path) == "nested") {
                                                              errno = error;
                                                              return -1;
                                                            }
                                                            return filemanager::openPropertyDescendant(fd, path);
                                                          }});
    assert(omitted.errors == 1 && omitted.partial() && !omitted.failed && omitted.files == 2);
    assert(omitted.diagnostics.size() == 1 && omitted.diagnostics.front().path == "nested");
    assert(omitted.diagnostics.front().error == error);
    assert(omitted.permissionErrors == (error == EACCES));
    assert(omitted.vanishedEntries == (error == ESTALE || error == ENOENT));
    assert(omitted.ioErrors == (error == EIO));
  }
  assert(denied.unreadableDirectories == 1 && denied.permissionErrors == 1);
  assert(boundary.diagnostics.size() == 1 && boundary.diagnostics.front().error == EXDEV);
  const auto missingRoot = filemanager::calculateFolderSize({fixture / "missing-root", 0, 0});
  assert(missingRoot.failed && missingRoot.partial() && !missingRoot.cancelled);
  assert(std::string_view(filemanager::folderSizeStatus(missingRoot)) == "failed");
  auto invalidRoot = target;
  ++invalidRoot.inode;
  assert(filemanager::calculateFolderSize(invalidRoot).failed);
  const auto enumerationTree = fixture / "enumeration-tree";
  std::filesystem::create_directory(enumerationTree);
  write(enumerationTree / "first", 1);
  write(enumerationTree / "second", 1);
  bool delivered = false;
  const auto enumerationFailure = filemanager::calculateFolderSize(
      request(enumerationTree), {}, {.readEntry = [&](DIR* directory) -> dirent* {
        if (delivered) {
          errno = EIO;
          return nullptr;
        }
        auto* entry = readdir(directory);
        if (entry && std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..")
          delivered = true;
        return entry;
      }}
  );
  assert(enumerationFailure.failed && enumerationFailure.partial() && enumerationFailure.files == 1);
  assert(enumerationFailure.bytes == 1 && enumerationFailure.ioErrors == 1);
  assert(enumerationFailure.diagnostics.front().operation == "read directory");
  const auto cycleTree = fixture / "cycle-tree";
  std::filesystem::create_directories(cycleTree / "loop");
  auto cycle = filemanager::calculateFolderSize(request(cycleTree), {}, {.openChild = [](int rootFd, const char*) {
                                                  return dup(rootFd);
                                                }});
  assert(cycle.partial() && cycle.errors == 1 && cycle.reason.find("cycle") != std::string::npos);
  const auto moved = fixture / "moved";
  std::filesystem::rename(root, moved);
  const auto renamed = request(moved);
  assert(renamed.inode == target.inode && renamed != target); // Path-bound completion tokens reject a renamed target.
  std::filesystem::create_directory(root);
  assert(filemanager::calculateFolderSize(target).partial()); // Replaced inode is never adopted.

  const auto semantics = fixture / "semantics";
  std::filesystem::create_directories(semantics / "denied");
  std::filesystem::create_directory(semantics / "accessible");
  write(semantics / "visible", 1);
  write(semantics / "denied/omitted", 9);
  std::filesystem::create_symlink("visible", semantics / "file-link");
  std::filesystem::create_directory_symlink("accessible", semantics / "folder-link");
  std::filesystem::create_symlink("missing", semantics / "broken-link");
  std::filesystem::create_hard_link(semantics / "visible", semantics / "another-name");
  assert(mkfifo((semantics / "pipe").c_str(), 0600) == 0);
  assert(chmod((semantics / "denied").c_str(), 0000) == 0);
  const auto compared = filemanager::calculateFolderSize(request(semantics));
  assert(compared.partial() && compared.errors == 1 && compared.permissionErrors == 1);
  assert(compared.files == 2 && compared.folders == 2 && compared.links == 3 && compared.other == 1);
  assert(
      compared.bytes
      == 2
          + std::string_view("visible").size()
          + std::string_view("accessible").size()
          + std::string_view("missing").size()
  );
  assert(chmod((semantics / "denied").c_str(), 0700) == 0);

  const auto large = fixture / "large";
  std::filesystem::create_directory(large);
  for (int i = 0; i < 2000; ++i)
    write(large / std::to_string(i), 1);
  const auto bounded = filemanager::calculateFolderSize(request(large), {}, {.openChild = [](int, const char*) {
                                                          errno = EIO;
                                                          return -1;
                                                        }});
  assert(bounded.errors == 2000 && bounded.diagnostics.size() == 16 && bounded.ioErrors == 2000);
  struct Async {
    filemanager::FolderSizeRequest target;
    GMainLoop* loop;
    std::thread::id main;
    std::mutex mutex;
    std::condition_variable ready;
    bool idleRan = false;
    bool complete = false;
  } async{request(large), g_main_loop_new(nullptr, false), std::this_thread::get_id()};
  auto* task = g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* value, gpointer data) {
        auto* state = static_cast<Async*>(data);
        std::unique_ptr<filemanager::FolderSizeResult> size(
            static_cast<filemanager::FolderSizeResult*>(g_task_propagate_pointer(G_TASK(value), nullptr))
        );
        assert(!size->partial() && size->bytes == 2000 && size->files == 2000);
        state->complete = true;
        g_main_loop_quit(state->loop);
      },
      &async
  );
  g_task_set_task_data(task, &async, nullptr);
  g_task_run_in_thread(task, [](GTask* worker, gpointer, gpointer data, GCancellable*) {
    auto* state = static_cast<Async*>(data);
    assert(std::this_thread::get_id() != state->main);
    std::unique_lock lock(state->mutex);
    assert(state->ready.wait_for(lock, std::chrono::seconds(5), [&] { return state->idleRan; }));
    lock.unlock();
    auto* size = new filemanager::FolderSizeResult(filemanager::calculateFolderSize(state->target));
    g_task_return_pointer(worker, size, [](gpointer value) {
      delete static_cast<filemanager::FolderSizeResult*>(value);
    });
  });
  g_idle_add(
      [](gpointer data) -> gboolean {
        auto* state = static_cast<Async*>(data);
        {
          const std::lock_guard lock(state->mutex);
          state->idleRan = true;
        }
        state->ready.notify_one();
        return G_SOURCE_REMOVE;
      },
      &async
  );
  g_main_loop_run(async.loop);
  assert(async.complete && async.idleRan);
  g_object_unref(task);
  g_main_loop_unref(async.loop);
  std::filesystem::remove_all(fixture);
}
