#include "config/atomic_file.h"
#include "config/state_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "state_store_test: {}", message);
    }
    return condition;
  }

  std::filesystem::path uniqueTestDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("noctalia-state-store-test-" + std::to_string(now));
  }

  constexpr std::filesystem::perms fileModeMask() {
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

  constexpr std::filesystem::perms ownerOnlyMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  std::filesystem::perms fileMode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & fileModeMask();
  }

  std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string out;
    char buffer[256]{};
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
      out.append(buffer, static_cast<std::size_t>(in.gcount()));
    }
    return out;
  }

  bool boolStateRoundTrips() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::remove_all(dir);

    StateStore store(path);
    store.load();

    bool ok = true;
    ok = expect(!store.boolValue("wallpaper_panel", "flatten").has_value(), "missing bool should be empty") && ok;
    ok = expect(store.setBool("wallpaper_panel", "flatten", true), "failed to write bool") && ok;

    const std::string content = readText(path);
    ok = expect(content.contains("[wallpaper_panel]"), "owner table was not written") && ok;
    ok = expect(content.contains("flatten = true"), "bool value was not written") && ok;

    StateStore loaded(path);
    loaded.load();
    ok = expect(loaded.boolValue("wallpaper_panel", "flatten").value_or(false), "bool did not reload") && ok;
    ok = expect(loaded.setBool("wallpaper_panel", "flatten", false), "failed to update bool") && ok;

    StateStore reloaded(path);
    reloaded.load();
    ok = expect(!reloaded.boolValue("wallpaper_panel", "flatten").value_or(true), "updated bool did not reload") && ok;
    ok = expect(!reloaded.setBool("wallpaper.panel", "flatten", true), "invalid owner was accepted") && ok;
    ok = expect(reloaded.clearOwner("wallpaper_panel"), "failed to clear owner table") && ok;
    ok = expect(!reloaded.boolValue("wallpaper_panel", "flatten").has_value(), "cleared owner remained visible") && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }

  bool stateFileIsOwnerOnlyOnCreate() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::remove_all(dir);

    StateStore store(path);
    store.load();

    bool ok = true;
    ok = expect(store.setString("calendar_credentials", "personal_password", "secret"), "failed to write string") && ok;
    ok = expect(fileMode(path) == ownerOnlyMode(), "new state file mode was not 0600") && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }

  bool stateLoadTightensExistingFile() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(path, std::ios::trunc);
      out << "[calendar_credentials]\npersonal_password = \"secret\"\n";
    }
    std::error_code ec;
    std::filesystem::permissions(
        path, ownerOnlyMode() | std::filesystem::perms::group_read | std::filesystem::perms::others_read,
        std::filesystem::perm_options::replace, ec
    );

    StateStore store(path);
    store.load();

    bool ok = true;
    ok = expect(fileMode(path) == ownerOnlyMode(), "existing state file mode was not tightened to 0600") && ok;
    ok = expect(
             store.stringValue("calendar_credentials", "personal_password").value_or("") == "secret",
             "secured state file did not load"
         )
        && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }

  bool wrongTypeIsNotReadAsBool() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(path, std::ios::trunc);
      out << "[wallpaper_panel]\nflatten = \"yes\"\n";
    }

    StateStore store(path);
    store.load();

    bool ok = true;
    ok = expect(!store.boolValue("wallpaper_panel", "flatten").has_value(), "wrong type was read as bool") && ok;
    ok = expect(store.setBool("wallpaper_panel", "flatten", true), "failed to replace wrong type") && ok;
    ok =
        expect(store.boolValue("wallpaper_panel", "flatten").value_or(false), "replacement bool was not visible") && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }

  bool atomicWritePreservesSymlink() {
    const auto dir = uniqueTestDir();
    const auto targetDir = dir / "dotfiles";
    const auto target = targetDir / "settings.toml";
    const auto link = dir / "settings.toml";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(targetDir);

    {
      std::ofstream out(target, std::ios::trunc);
      out << "[theme]\nmode = \"dark\"\n";
    }
    std::filesystem::create_symlink(target, link);

    bool ok = true;
    ok = expect(writeTextFileAtomic(link, "[theme]\nmode = \"light\"\n"), "failed to write symlinked file") && ok;
    ok = expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link)), "settings symlink was replaced")
        && ok;
    ok = expect(readText(target).contains("mode = \"light\""), "settings target content was not updated") && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }

  bool stateStorePreservesSymlink() {
    const auto dir = uniqueTestDir();
    const auto targetDir = dir / "dotfiles";
    const auto target = targetDir / "state.toml";
    const auto link = dir / "state.toml";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(targetDir);

    {
      std::ofstream out(target, std::ios::trunc);
      out << "[wallpaper_panel]\nflatten = false\n";
    }
    std::filesystem::create_symlink(target, link);

    StateStore store(link);
    store.load();

    bool ok = true;
    ok = expect(store.setBool("wallpaper_panel", "flatten", true), "failed to update symlinked state") && ok;
    ok = expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link)), "state symlink was replaced") && ok;
    ok = expect(readText(target).contains("flatten = true"), "state target content was not updated") && ok;
    ok = expect(fileMode(target) == ownerOnlyMode(), "state symlink target mode was not 0600") && ok;

    std::filesystem::remove_all(dir);
    return ok;
  }
} // namespace

int main() {
  bool ok = true;
  ok = boolStateRoundTrips() && ok;
  ok = stateFileIsOwnerOnlyOnCreate() && ok;
  ok = stateLoadTightensExistingFile() && ok;
  ok = wrongTypeIsNotReadAsBool() && ok;
  ok = atomicWritePreservesSymlink() && ok;
  ok = stateStorePreservesSymlink() && ok;
  return ok ? 0 : 1;
}
