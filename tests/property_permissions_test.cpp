#include "filemanager/property_permissions.h"

#include <cassert>
#include <fstream>
#include <gio/gio.h>

int main() {
  char* raw = g_dir_make_tmp("nythera-properties-test-XXXXXX", nullptr);
  assert(raw);
  const std::filesystem::path root(raw);
  g_free(raw);
  const auto file = root / "file";
  std::ofstream(file) << "fixture";
  struct stat original{};
  assert(chmod(file.c_str(), 0740) == 0);
  assert(lstat(file.c_str(), &original) == 0);
  const auto apply = [&](const std::filesystem::path& path, mode_t from, mode_t to) {
    return filemanager::applyPropertyPermissions(path, original.st_dev, original.st_ino, from, to);
  };
  const auto mode = [](const std::filesystem::path& path) {
    struct stat state{};
    assert(lstat(path.c_str(), &state) == 0);
    return state.st_mode & 07777;
  };
  assert(apply(file, 0740, 0644).error.empty());
  assert(mode(file) == 0644);

  // Preserve concurrently changed bits that were not edited, including special bits.
  assert(chmod(file.c_str(), 07640) == 0);
  assert(apply(file, 0640, 0644).error.empty());
  assert(mode(file) == 07644);
  assert(apply(file, 0644, 0644).error.empty());
  assert(mode(file) == 07644);

  // Owners can chmod even without read or write access to file contents.
  assert(chmod(file.c_str(), 0000) == 0);
  assert(apply(file, 0000, 0600).error.empty());
  assert(mode(file) == 0600);

  const auto link = root / "link";
  std::filesystem::create_symlink(file, link);
  assert(!apply(link, 0600, 0777).error.empty());
  assert(mode(file) == 0600);
  std::filesystem::create_symlink(root / "missing", root / "broken");
  assert(!apply(root / "broken", 0600, 0777).error.empty());
  assert(!apply(root / "missing", 0600, 0777).error.empty());

  // The path may disappear, be replaced, or become a symlink while Properties is open.
  std::filesystem::rename(file, root / "old");
  std::ofstream(file) << "replacement";
  assert(chmod(file.c_str(), 0640) == 0);
  assert(!apply(file, 0600, 0777).error.empty());
  assert(mode(file) == 0640 && mode(root / "old") == 0600);
  std::filesystem::remove(file);
  std::filesystem::create_symlink(root / "old", file);
  assert(!apply(file, 0600, 0777).error.empty());
  assert(mode(root / "old") == 0600);

  const auto directory = root / "directory";
  std::filesystem::create_directory(directory);
  std::ofstream(directory / "child") << "child";
  assert(chmod(directory.c_str(), 0750) == 0);
  assert(chmod((directory / "child").c_str(), 0640) == 0);
  struct stat folder{};
  assert(lstat(directory.c_str(), &folder) == 0);
  assert(filemanager::applyPropertyPermissions(directory, folder.st_dev, folder.st_ino, 0750, 0700).error.empty());
  assert(mode(directory) == 0700 && mode(directory / "child") == 0640);

  const auto fifo = root / "fifo";
  assert(mkfifo(fifo.c_str(), 0600) == 0);
  struct stat special{};
  assert(lstat(fifo.c_str(), &special) == 0);
  assert(!filemanager::applyPropertyPermissions(fifo, special.st_dev, special.st_ino, 0600, 0777).error.empty());
  assert(mode(fifo) == 0600);
  std::filesystem::remove_all(root);
}
