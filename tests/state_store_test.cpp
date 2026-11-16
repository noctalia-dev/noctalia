#include "config/atomic_file.h"
#include "config/state_store.h"
#include "tests/test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
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

  void boolStateRoundTrips() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::remove_all(dir);

    StateStore store(path);
    store.load();

    const bool missingEmpty = !store.boolValue("wallpaper_panel", "flatten").has_value();
    const bool wrote = store.setBool("wallpaper_panel", "flatten", true);

    const std::string content = readText(path);

    StateStore loaded(path);
    loaded.load();
    const bool reloaded1 = loaded.boolValue("wallpaper_panel", "flatten").value_or(false);
    const bool updated = loaded.setBool("wallpaper_panel", "flatten", false);

    StateStore reloaded(path);
    reloaded.load();
    const bool reloaded2 = !reloaded.boolValue("wallpaper_panel", "flatten").value_or(true);
    const bool invalidOwnerRejected = !reloaded.setBool("wallpaper.panel", "flatten", true);
    const bool cleared = reloaded.clearOwner("wallpaper_panel");
    const bool clearedGone = !reloaded.boolValue("wallpaper_panel", "flatten").has_value();

    std::filesystem::remove_all(dir);

    TEST_CHECK(missingEmpty);
    TEST_CHECK(wrote);
    TEST_CHECK(content.contains("[wallpaper_panel]"));
    TEST_CHECK(content.contains("flatten = true"));
    TEST_CHECK(reloaded1);
    TEST_CHECK(updated);
    TEST_CHECK(reloaded2);
    TEST_CHECK(invalidOwnerRejected);
    TEST_CHECK(cleared);
    TEST_CHECK(clearedGone);
  }

  void stringArraysRoundTripAndRejectInvalidState() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    const std::vector<std::string> keys = {"official:CaseAuthor/Plugin", "community:CaseAuthor/Plugin"};
    StateStore store(path);
    TEST_CHECK(!store.stringArrayValue("plugin_store", "recommended_plugin_keys").has_value());
    TEST_CHECK(store.setStringArray("plugin_store", "recommended_plugin_keys", keys));
    TEST_CHECK(fileMode(path) == ownerOnlyMode());
    StateStore loaded(path);
    loaded.load();
    TEST_CHECK(loaded.stringArrayValue("plugin_store", "recommended_plugin_keys") == keys);
    TEST_CHECK(loaded.setStringArray("plugin_store", "recommended_plugin_keys", {}));
    loaded.load();
    TEST_CHECK(loaded.stringArrayValue("plugin_store", "recommended_plugin_keys") == std::vector<std::string>{});
    {
      std::ofstream out(path, std::ios::trunc);
      out << "[plugin_store]\nrecommended_plugin_keys = [\"official:CaseAuthor/Plugin\", 1]\n";
    }
    loaded.load();
    TEST_CHECK(loaded.contains("plugin_store", "recommended_plugin_keys"));
    TEST_CHECK(!loaded.stringArrayValue("plugin_store", "recommended_plugin_keys").has_value());
    {
      std::ofstream out(path, std::ios::trunc);
      out << "plugin_store = 7\n";
    }
    loaded.load();
    TEST_CHECK(!loaded.ownerValid("plugin_store"));

    TEST_CHECK(store.setString("plugin_store", "recommendation_secret", "persisted"));
    // A directory at the atomic target forces an IO failure without changing permissions or user state.
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    TEST_CHECK(!store.setString("plugin_store", "recommendation_secret", "unsaved"));
    TEST_CHECK(store.stringValue("plugin_store", "recommendation_secret") == "persisted");
    TEST_CHECK(!store.setStringArray("plugin_store", "recommended_plugin_keys", {}));
    TEST_CHECK(store.stringArrayValue("plugin_store", "recommended_plugin_keys") == keys);
    std::filesystem::remove_all(dir);
  }

  void stateFileIsOwnerOnlyOnCreate() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::remove_all(dir);

    StateStore store(path);
    store.load();

    const bool wrote = store.setString("calendar_credentials", "personal_password", "secret");
    const auto mode = fileMode(path);

    std::filesystem::remove_all(dir);

    TEST_CHECK(wrote);
    TEST_CHECK(mode == ownerOnlyMode());
  }

  void stateLoadTightensExistingFile() {
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

    const auto mode = fileMode(path);
    const std::string password = store.stringValue("calendar_credentials", "personal_password").value_or("");

    std::filesystem::remove_all(dir);

    TEST_CHECK(mode == ownerOnlyMode());
    TEST_CHECK(password == "secret");
  }

  void wrongTypeIsNotReadAsBool() {
    const auto dir = uniqueTestDir();
    const auto path = dir / "state.toml";
    std::filesystem::create_directories(dir);
    {
      std::ofstream out(path, std::ios::trunc);
      out << "[wallpaper_panel]\nflatten = \"yes\"\n";
    }

    StateStore store(path);
    store.load();

    const bool wrongTypeEmpty = !store.boolValue("wallpaper_panel", "flatten").has_value();
    const bool replaced = store.setBool("wallpaper_panel", "flatten", true);
    const bool replacementVisible = store.boolValue("wallpaper_panel", "flatten").value_or(false);

    std::filesystem::remove_all(dir);

    TEST_CHECK(wrongTypeEmpty);
    TEST_CHECK(replaced);
    TEST_CHECK(replacementVisible);
  }

  void atomicWritePreservesSymlink() {
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

    const bool wrote = writeTextFileAtomic(link, "[theme]\nmode = \"light\"\n");
    const bool stillSymlink = std::filesystem::is_symlink(std::filesystem::symlink_status(link));
    const std::string targetContent = readText(target);

    std::filesystem::remove_all(dir);

    TEST_CHECK(wrote);
    TEST_CHECK(stillSymlink);
    TEST_CHECK(targetContent.contains("mode = \"light\""));
  }

  void stateStorePreservesSymlink() {
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

    const bool wrote = store.setBool("wallpaper_panel", "flatten", true);
    const bool stillSymlink = std::filesystem::is_symlink(std::filesystem::symlink_status(link));
    const std::string targetContent = readText(target);
    const auto targetMode = fileMode(target);

    std::filesystem::remove_all(dir);

    TEST_CHECK(wrote);
    TEST_CHECK(stillSymlink);
    TEST_CHECK(targetContent.contains("flatten = true"));
    TEST_CHECK(targetMode == ownerOnlyMode());
  }

} // namespace

int main() {
  stringArraysRoundTripAndRejectInvalidState();
  boolStateRoundTrips();
  stateFileIsOwnerOnlyOnCreate();
  stateLoadTightensExistingFile();
  wrongTypeIsNotReadAsBool();
  atomicWritePreservesSymlink();
  stateStorePreservesSymlink();
  return 0;
}
