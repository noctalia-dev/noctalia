#include "filemanager/property_operations.h"

#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <gio/gdesktopappinfo.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace filemanager;
namespace {
  mode_t mode(const std::filesystem::path& path) {
    struct stat state{};
    assert(lstat(path.c_str(), &state) == 0);
    return state.st_mode & 07777;
  }
  PropertyRequest request(const std::filesystem::path& path, mode_t edited, bool recursive = false) {
    struct stat state{};
    assert(lstat(path.c_str(), &state) == 0);
    return {path,     static_cast<std::uint64_t>(state.st_dev), state.st_ino, state.st_mode & 07777U, edited, {}, {},
            recursive};
  }
  void write(const std::filesystem::path& path, mode_t bits) {
    std::ofstream(path) << "fixture";
    assert(chmod(path.c_str(), bits) == 0);
  }
} // namespace
int main() {
  setenv("GIO_USE_VFS", "local", 1);
  assert(geteuid() != 0); // This test never needs or exercises actual elevated privileges.
  char* raw = g_dir_make_tmp("nythera-property-ops-XXXXXX", nullptr);
  assert(raw);
  const std::filesystem::path root(raw);
  g_free(raw);
  const auto dataHome = root / "data";
  const auto configHome = root / "config";
  const auto cacheHome = root / "cache";
  std::filesystem::create_directories(dataHome / "applications");
  std::filesystem::create_directories(configHome);
  std::filesystem::create_directories(cacheHome);
  setenv("XDG_DATA_HOME", dataHome.c_str(), 1);
  setenv("XDG_DATA_DIRS", dataHome.c_str(), 1);
  setenv("XDG_CONFIG_HOME", configHome.c_str(), 1);
  setenv("XDG_CONFIG_DIRS", configHome.c_str(), 1);
  setenv("XDG_CACHE_HOME", cacheHome.c_str(), 1);
  const auto file = root / "file";
  write(file, 0600);
  auto edit = request(file, 0640);
  auto args = propertyArguments(edit);
  PropertyRequest parsed;
  assert(parsePropertyArguments(args, parsed) && parsed.path == file && parsed.edited == 0640);
  for (std::size_t i : {1U, 2U, 3U, 4U, 7U}) {
    auto invalid = args;
    invalid[i] = "-1;touch /tmp/unchecked";
    assert(!parsePropertyArguments(invalid, parsed));
  }
  auto invalid = args;
  invalid[0] += "?";
  assert(!parsePropertyArguments(invalid, parsed));
  invalid = args;
  invalid.pop_back();
  assert(!parsePropertyArguments(invalid, parsed));
  auto malformed = edit;
  malformed.path = "relative";
  assert(!validatePropertyRequest(malformed).empty());
  malformed = edit;
  malformed.edited |= 04000;
  assert(!validatePropertyRequest(malformed).empty());
  malformed = edit;
  malformed.owner = "__nythera_missing_account_987654__";
  assert(!validatePropertyRequest(malformed).empty());

  const auto accounts = propertyAccounts();
  assert(!accounts.users.empty() && !accounts.groups.empty());
  std::uint32_t id = 0;
  for (const auto& user : accounts.users) {
    assert(resolvePropertyIdentity(user.name, false, id));
    assert(id == user.id);
  }
  bool ownGroup = false;
  for (const auto& group : accounts.groups) {
    assert(resolvePropertyIdentity(group.name, true, id) && id == group.id);
    assert(group.eligible == propertyGroupEligible(group.id));
    if (group.id == getegid())
      ownGroup = group.eligible;
  }
  assert(accounts.users.front().id == geteuid());
  bool sawIneligibleGroup = false;
  for (const auto& group : accounts.groups) {
    if (!group.eligible)
      sawIneligibleGroup = true;
    else
      assert(!sawIneligibleGroup);
  }
  assert(ownGroup);
  assert(!resolvePropertyIdentity("0; chmod", false, id));

  // Staging does not change the filesystem, and concurrent unedited bits survive Apply.
  assert(mode(file) == 0600);
  assert(chmod(file.c_str(), 04700) == 0);
  auto applied = executePropertyRequest(edit);
  assert(applied.ok() && applied.succeeded == 1 && mode(file) == 04740);
  // Re-query the same metadata attributes used by Properties after completion.
  GFile* gioFile = g_file_new_for_path(file.c_str());
  GFileInfo* refreshed = g_file_query_info(
      gioFile, "unix::mode,unix::uid,unix::gid", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
  );
  assert(refreshed && (g_file_info_get_attribute_uint32(refreshed, G_FILE_ATTRIBUTE_UNIX_MODE) & 07777) == 04740);
  assert(g_file_info_get_attribute_uint32(refreshed, G_FILE_ATTRIBUTE_UNIX_UID) == geteuid());
  g_object_unref(refreshed);
  g_object_unref(gioFile);

  assert(chmod(file.c_str(), 0000) == 0);
  assert(executePropertyRequest(request(file, 0600)).ok() && mode(file) == 0600);
  assert(chmod(file.c_str(), 04740) == 0);

  // Group changes preserve the owner; the kernel's special-bit effects are reported.
  auto groupEdit = request(file, mode(file));
  for (const auto& group : accounts.groups)
    if (group.id == getegid())
      groupEdit.group = group.name;
  const auto before = mode(file);
  auto groupResult = executePropertyRequest(groupEdit);
  assert(groupResult.ok());
  struct stat state{};
  assert(lstat(file.c_str(), &state) == 0);
  assert(state.st_uid == geteuid() && state.st_gid == getegid());
  assert(groupResult.ownershipEffects == (((before & 06000) & ~mode(file)) != 0));

  // Denial/cancellation are injected; no helper, pkexec or authentication prompt is invoked.
  GCancellable* cancellable = g_cancellable_new();
  bool authorised = false;
  auto recursiveMock = edit;
  recursiveMock.recursive = false;
  auto denied = runPropertyRequest(recursiveMock, geteuid() + 1, cancellable, {}, [&](const auto&, auto*, auto) {
    authorised = true;
    return propertyAuthorizationFailure(127, false);
  });
  assert(authorised && !denied.ok() && !denied.cancelled);
  assert(propertyAuthorizationFailure(126, false).cancelled);
  g_cancellable_cancel(cancellable);
  authorised = false;
  auto stopped = runPropertyRequest(recursiveMock, geteuid() + 1, cancellable, {}, [&](const auto&, auto*, auto) {
    authorised = true;
    return PropertyResult{};
  });
  assert(stopped.cancelled && !authorised);
  g_object_unref(cancellable);

  cancellable = g_cancellable_new();
  auto authCancelled = runPropertyRequest(recursiveMock, geteuid() + 1, cancellable, {}, [&](const auto&, auto*, auto) {
    return propertyAuthorizationFailure(126, false);
  });
  assert(authCancelled.cancelled);
  g_object_unref(cancellable);

  // Default application persistence is restricted to this process's isolated XDG tree.
  const auto desktop = dataHome / "applications/nythera-properties-fixture.desktop";
  std::ofstream(
      desktop
  ) << "[Desktop Entry]\nType=Application\nName=Properties test app\nExec=/bin/true %f\nMimeType=text/plain;\n";
  std::ofstream(dataHome / "applications/mimeinfo.cache")
      << "[MIME Cache]\ntext/plain=nythera-properties-fixture.desktop;\n";
  GDesktopAppInfo* app = g_desktop_app_info_new_from_filename(desktop.c_str());
  assert(app);
  GError* appError = nullptr;
  assert(savePropertyDefaultApplication(G_APP_INFO(app), "text/plain", &appError));
  assert(!appError && std::filesystem::exists(configHome / "mimeapps.list"));
  GAppInfo* defaultApp = g_app_info_get_default_for_type("text/plain", FALSE);
  assert(defaultApp && g_app_info_equal(defaultApp, G_APP_INFO(app)));
  assert(!savePropertyDefaultApplication(G_APP_INFO(app), "image/png", &appError));
  assert(appError);
  g_clear_error(&appError);
  g_object_unref(defaultApp);
  g_object_unref(app);

  // File identity, terminal and ancestor symlinks, broken links and path substitution.
  std::filesystem::create_symlink(file, root / "link");
  auto link = request(root / "link", 0644);
  assert(!executePropertyRequest(link).ok());
  std::filesystem::create_symlink(root / "absent", root / "broken");
  assert(!executePropertyRequest(request(root / "broken", 0644)).ok());
  std::filesystem::rename(file, root / "old");
  write(file, 0600);
  assert(!executePropertyRequest(edit).ok() && mode(file) == 0600);
  std::filesystem::create_directory(root / "actual");
  write(root / "actual/child", 0600);
  std::filesystem::create_directory_symlink(root / "actual", root / "alias");
  assert(!executePropertyRequest(request(root / "alias/child", 0644)).ok());

  // Recursive masks preserve special bits and regular-file execute bits.
  const auto tree = root / "tree";
  std::filesystem::create_directories(tree / "nested");
  assert(chmod(tree.c_str(), 0700) == 0 && chmod((tree / "nested").c_str(), 01700) == 0);
  write(tree / "plain", 0640);
  write(tree / "executable", 0750);
  write(root / "outside", 0600);
  std::filesystem::create_symlink(root / "outside", tree / "outside-link");
  std::filesystem::create_symlink(root / "absent", tree / "broken");
  std::filesystem::create_hard_link(root / "outside", tree / "hard-link");
  auto recursive = request(tree, 0755, true);
  auto result = executePropertyRequest(recursive);
  assert(result.ok() && result.succeeded == 4 && result.skipped == 3);
  assert(mode(tree) == 0755 && mode(tree / "nested") == 01755);
  assert(mode(tree / "plain") == 0644 && mode(tree / "executable") == 0754 && mode(root / "outside") == 0600);
  auto illegal = recursive;
  illegal.owner = accounts.users.front().name;
  assert(!validatePropertyRequest(illegal).empty());

  // Kernel-enforced lexical/mount boundaries, including a real existing read-only mount lookup.
  int treeFd = open(tree.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  assert(treeFd >= 0);
  assert(openPropertyDescendant(treeFd, "../outside") < 0 && errno == EXDEV);
  assert(openPropertyDescendant(treeFd, "/etc/passwd") < 0 && errno == EXDEV);
  close(treeFd);
  int slash = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
  assert(slash >= 0);
  assert(openPropertyDescendant(slash, "proc") < 0 && errno == EXDEV);
  close(slash);

  // Bounded traversal, partial failures, cancellation and duplicate-operation leases.
  const auto limited = root / "limited";
  std::filesystem::create_directories(limited / "nested");
  write(limited / "nested/untouched", 0600);
  auto depth = executePropertyRequest(request(limited, 0704, true), {}, {}, {.depth = 1});
  assert(!depth.ok() && depth.failed > 0 && mode(limited / "nested/untouched") == 0600);
  const auto cancelTree = root / "cancel";
  std::filesystem::create_directory(cancelTree);
  assert(chmod(cancelTree.c_str(), 0700) == 0);
  write(cancelTree / "one", 0600);
  write(cancelTree / "two", 0600);
  bool cancel = false, duplicate = false;
  const auto cancelRequest = request(cancelTree, mode(cancelTree) | 0004, true);
  auto cancelled = executePropertyRequest(
      cancelRequest, [&] { return cancel; },
      [&](const auto& update) {
        if (update.succeeded == 1 && !cancel) {
          duplicate = !executePropertyRequest(cancelRequest).ok();
          cancel = true;
        }
      }
  );
  assert(cancelled.cancelled && cancelled.succeeded == 1 && duplicate);
  assert((mode(cancelTree / "one") == 0604) != (mode(cancelTree / "two") == 0604));
  assert(cancelled.summary().find("not rolled back") != std::string::npos);
  auto entryLimit = executePropertyRequest(cancelRequest, {}, {}, {.entries = 1});
  assert(!entryLimit.ok() && entryLimit.failed > 0 && entryLimit.succeeded <= 1);
  const auto blocked = root / "blocked";
  std::filesystem::create_directories(blocked / "inaccessible");
  write(blocked / "accessible", 0600);
  write(blocked / "inaccessible/unchanged", 0600);
  assert(chmod(blocked.c_str(), 0700) == 0 && chmod((blocked / "inaccessible").c_str(), 0000) == 0);
  cancellable = g_cancellable_new();
  bool preflightAuthorised = false;
  auto blockedRequest = request(blocked, 0704, true);
  auto preflightDenied =
      runPropertyRequest(blockedRequest, geteuid(), cancellable, {}, [&](const auto& scoped, auto*, auto) {
        assert(scoped.path == blocked && scoped.recursive);
        preflightAuthorised = true;
        return propertyAuthorizationFailure(127, false);
      });
  assert(preflightAuthorised && !preflightDenied.ok() && mode(blocked) == 0700);
  // Readable but unsearchable directories also require one scoped authorisation.
  assert(chmod((blocked / "inaccessible").c_str(), 0400) == 0);
  preflightAuthorised = false;
  auto noTraversal = runPropertyRequest(blockedRequest, geteuid(), cancellable, {}, [&](const auto&, auto*, auto) {
    preflightAuthorised = true;
    return propertyAuthorizationFailure(127, false);
  });
  assert(preflightAuthorised && !noTraversal.ok() && mode(blocked) == 0700);
  assert(chmod((blocked / "inaccessible").c_str(), 0000) == 0);
  g_object_unref(cancellable);
  auto partial = executePropertyRequest(request(blocked, 0704, true));
  assert(!partial.ok() && partial.failed == 1 && partial.succeeded == 2);
  assert(chmod((blocked / "inaccessible").c_str(), 0700) == 0);
  assert(mode(blocked / "inaccessible/unchanged") == 0600);

  const auto ownedTree = root / "owned-preflight";
  std::filesystem::create_directory(ownedTree);
  assert(chmod(ownedTree.c_str(), 0700) == 0);
  cancellable = g_cancellable_new();
  bool unnecessaryAuth = false;
  auto ownedResult =
      runPropertyRequest(request(ownedTree, 0704, true), geteuid(), cancellable, {}, [&](const auto&, auto*, auto) {
        unnecessaryAuth = true;
        return propertyAuthorizationFailure(127, false);
      });
  assert(ownedResult.ok() && !unnecessaryAuth && mode(ownedTree) == 0704);
  g_object_unref(cancellable);

  // A child replaced by a symlink after traversal starts cannot redirect subsequent chmod.
  bool replaced = false;
  auto substitution = executePropertyRequest(request(cancelTree, 0700, true), {}, [&](const auto&) {
    if (replaced)
      return;
    replaced = true;
    std::filesystem::rename(cancelTree / "two", root / "moved-two");
    std::filesystem::create_symlink(root / "outside", cancelTree / "two");
  });
  assert(mode(root / "outside") == 0600);
  (void)substitution;
  std::filesystem::permissions(tree / "nested", std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
  std::filesystem::remove_all(root);
}
