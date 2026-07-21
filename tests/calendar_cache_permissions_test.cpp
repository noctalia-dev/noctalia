#include "calendar/calendar_cache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

namespace {
  constexpr std::filesystem::perms permissionMask() {
    using P = std::filesystem::perms;
    return P::owner_read
        | P::owner_write
        | P::owner_exec
        | P::group_read
        | P::group_write
        | P::group_exec
        | P::others_read
        | P::others_write
        | P::others_exec;
  }

  constexpr std::filesystem::perms privateFileMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  constexpr std::filesystem::perms privateDirectoryMode() {
    return privateFileMode() | std::filesystem::perms::owner_exec;
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_cache_permissions_test: {}", message);
    }
    return condition;
  }

  std::filesystem::perms mode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & permissionMask();
  }
} // namespace

int main() {
  namespace fs = std::filesystem;
  using P = fs::perms;

  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("noctalia-calendar-cache-test-" + std::to_string(serial));
  const fs::path cacheDir = root / "noctalia/calendar";
  const fs::path cachePath = cacheDir / "events.json";

  fs::remove_all(root);
  fs::create_directories(cacheDir);
  {
    std::ofstream out(cachePath, std::ios::trunc);
    out << R"({"events":[]})";
  }
  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  fs::permissions(
      cachePath, privateFileMode() | P::group_read | P::others_read, fs::perm_options::replace
  );

  bool ok = true;
  ok = expect(calendar::cache::secureExisting(cachePath), "failed to secure existing cache") && ok;
  ok = expect(mode(cacheDir) == privateDirectoryMode(), "existing cache directory mode was not 0700") && ok;
  ok = expect(mode(cachePath) == privateFileMode(), "existing cache file mode was not 0600") && ok;

  fs::remove(cachePath);
  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  ok = expect(calendar::cache::write(cachePath, R"({"events":[]})"), "failed to write cache") && ok;
  ok = expect(mode(cacheDir) == privateDirectoryMode(), "written cache directory mode was not 0700") && ok;
  ok = expect(mode(cachePath) == privateFileMode(), "written cache file mode was not 0600") && ok;

  fs::remove_all(root);
  return ok ? 0 : 1;
}
