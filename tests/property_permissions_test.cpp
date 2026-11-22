#include "filemanager/property_permissions.h"
#include "test_isolation.h"

#include <fstream>
#include <gio/gio.h>

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-properties-test"});
    const auto& root = env.work();
    const auto file = root / "file";
    std::ofstream(file) << "fixture";
    struct stat original{};
    TEST_CHECK(chmod(file.c_str(), 0740) == 0);
    TEST_CHECK(lstat(file.c_str(), &original) == 0);
    const auto apply = [&](const std::filesystem::path& path, mode_t from, mode_t to) {
      return filemanager::applyPropertyPermissions(path, original.st_dev, original.st_ino, from, to);
    };
    const auto mode = [](const std::filesystem::path& path) {
      struct stat state{};
      TEST_CHECK(lstat(path.c_str(), &state) == 0);
      return state.st_mode & 07777;
    };
    TEST_CHECK(apply(file, 0740, 0644).error.empty());
    TEST_CHECK(mode(file) == 0644);

    // Preserve concurrently changed bits that were not edited, including special bits.
    TEST_CHECK(chmod(file.c_str(), 07640) == 0);
    TEST_CHECK(apply(file, 0640, 0644).error.empty());
    TEST_CHECK(mode(file) == 07644);
    TEST_CHECK(apply(file, 0644, 0644).error.empty());
    TEST_CHECK(mode(file) == 07644);

    // Owners can chmod even without read or write access to file contents.
    TEST_CHECK(chmod(file.c_str(), 0000) == 0);
    TEST_CHECK(apply(file, 0000, 0600).error.empty());
    TEST_CHECK(mode(file) == 0600);

    const auto link = root / "link";
    std::filesystem::create_symlink(file, link);
    TEST_CHECK(!apply(link, 0600, 0777).error.empty());
    TEST_CHECK(mode(file) == 0600);
    std::filesystem::create_symlink(root / "missing", root / "broken");
    TEST_CHECK(!apply(root / "broken", 0600, 0777).error.empty());
    TEST_CHECK(!apply(root / "missing", 0600, 0777).error.empty());

    // The path may disappear, be replaced, or become a symlink while Properties is open.
    std::filesystem::rename(file, root / "old");
    std::ofstream(file) << "replacement";
    TEST_CHECK(chmod(file.c_str(), 0640) == 0);
    TEST_CHECK(!apply(file, 0600, 0777).error.empty());
    TEST_CHECK(mode(file) == 0640 && mode(root / "old") == 0600);
    std::filesystem::remove(file);
    std::filesystem::create_symlink(root / "old", file);
    TEST_CHECK(!apply(file, 0600, 0777).error.empty());
    TEST_CHECK(mode(root / "old") == 0600);

    const auto directory = root / "directory";
    std::filesystem::create_directory(directory);
    std::ofstream(directory / "child") << "child";
    TEST_CHECK(chmod(directory.c_str(), 0750) == 0);
    TEST_CHECK(chmod((directory / "child").c_str(), 0640) == 0);
    struct stat folder{};
    TEST_CHECK(lstat(directory.c_str(), &folder) == 0);
    TEST_CHECK(
        filemanager::applyPropertyPermissions(directory, folder.st_dev, folder.st_ino, 0750, 0700).error.empty()
    );
    TEST_CHECK(mode(directory) == 0700 && mode(directory / "child") == 0640);

    const auto fifo = root / "fifo";
    TEST_CHECK(mkfifo(fifo.c_str(), 0600) == 0);
    struct stat special{};
    TEST_CHECK(lstat(fifo.c_str(), &special) == 0);
    TEST_CHECK(!filemanager::applyPropertyPermissions(fifo, special.st_dev, special.st_ino, 0600, 0777).error.empty());
    TEST_CHECK(mode(fifo) == 0600);
  });
}
