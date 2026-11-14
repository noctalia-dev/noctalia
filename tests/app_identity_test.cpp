#include "system/app_identity.h"
#include "system/internal_app_metadata.h"
#include "tests/test_check.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace internal_apps {

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

namespace {

  DesktopEntry sampleChatEntry() {
    DesktopEntry entry;
    entry.id = "sample-chat-desktop";
    entry.name = "Sample Chat";
    entry.nameLower = "sample chat";
    entry.startupWmClass = "SampleChat";
    entry.startupWmClassLower = "samplechat";
    entry.exec = "sample-chat-desktop";
    entry.icon = "sample-chat-desktop";
    return entry;
  }

  DesktopEntry sampleMailEntry() {
    DesktopEntry entry;
    entry.id = "sample-mail";
    entry.name = "Sample Mail";
    entry.nameLower = "sample mail";
    entry.startupWmClass = "SampleMail";
    entry.startupWmClassLower = "samplemail";
    entry.exec = "sample-mail";
    entry.icon = "sample-mail";
    return entry;
  }

  DesktopEntry displayNameOnlyEntry() {
    DesktopEntry entry;
    entry.id = "other-app";
    entry.name = "Risky Match";
    entry.nameLower = "risky match";
    entry.startupWmClass = "OtherApp";
    entry.startupWmClassLower = "otherapp";
    entry.exec = "other-app";
    entry.icon = "other-app";
    return entry;
  }

  void expectMatch(const DesktopEntry& entry, std::string_view token) {
    TEST_CHECK(app_identity::desktopEntryMatchesLower(entry, token));
  }

  void expectNoMatch(const DesktopEntry& entry, std::string_view token) {
    TEST_CHECK(!app_identity::desktopEntryMatchesLower(entry, token));
  }

  DesktopEntry easyEffectsEntry() {
    DesktopEntry entry;
    entry.id = "com.github.wwmm.easyeffects";
    entry.name = "Easy Effects";
    entry.nameLower = "easy effects";
    entry.startupWmClass = "easyeffects";
    entry.startupWmClassLower = "easyeffects";
    entry.exec = "easyeffects";
    entry.icon = "easyeffects";
    return entry;
  }

  DesktopEntry duplicateTailEntry(std::string_view id, std::string_view startupWmClass) {
    DesktopEntry entry;
    entry.id = std::string(id);
    entry.name = std::string(id);
    entry.nameLower = StringUtils::toLower(entry.name);
    entry.startupWmClass = std::string(startupWmClass);
    entry.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
    entry.exec = entry.id;
    entry.icon = entry.id;
    return entry;
  }

  DesktopEntry issueEntry(std::string_view id, std::string_view name, std::string_view icon) {
    DesktopEntry entry;
    entry.id = std::string(id);
    entry.idLower = StringUtils::toLower(entry.id);
    entry.name = std::string(name);
    entry.nameLower = StringUtils::toLower(entry.name);
    entry.icon = std::string(icon);
    return entry;
  }

  void testSharedNamespaceDoesNotIdentifyApplication() {
    const DesktopEntry noctalia = issueEntry("dev.noctalia.Noctalia", "Noctalia", "noctalia");
    const DesktopEntry luciole = issueEntry("dev.noctalia.luciole", "Luciole", "luciole");
    const auto shellOnly = std::array<DesktopEntry, 1>{noctalia};
    TEST_CHECK(!app_identity::findDesktopEntry(luciole.id, shellOnly));
    const DesktopEntry unresolved = app_identity::resolveRunningDesktopEntry(luciole.id, shellOnly);
    TEST_CHECK(unresolved.id == luciole.id && unresolved.name == luciole.id);
    TEST_CHECK(unresolved.icon.empty() && unresolved.exec.empty());

    const auto running =
        app_identity::resolveRunningApps(std::array<std::string, 2>{noctalia.id, luciole.id}, shellOnly);
    TEST_CHECK(running.size() == 2);
    TEST_CHECK(running[0].entry.id == noctalia.id && running[1].entry.id == luciole.id);
    TEST_CHECK(running[0].entry.icon == noctalia.icon && running[1].entry.icon.empty());

    const auto installed = std::array<DesktopEntry, 2>{noctalia, luciole};
    const auto matched = app_identity::findDesktopEntry(luciole.id, installed, shellOnly);
    TEST_CHECK(matched && matched->id == luciole.id && matched->icon == luciole.icon);
    TEST_CHECK(!app_identity::findDesktopEntry(noctalia.id, std::array<DesktopEntry, 1>{luciole}));

    auto terminal = issueEntry("terminal", "Terminal", "terminal");
    terminal.startupWmClass = "dev.noctalia.Terminal";
    TEST_CHECK(!app_identity::findDesktopEntry(luciole.id, std::array<DesktopEntry, 1>{terminal}));
    TEST_CHECK(!app_identity::findDesktopEntry("noctalia", std::array<DesktopEntry, 1>{terminal}));
    TEST_CHECK(!app_identity::findDesktopEntry("luciole", shellOnly));

    const auto calculator = issueEntry("org.gnome.Calculator", "Calculator", "calculator");
    TEST_CHECK(!app_identity::findDesktopEntry("org.gnome.Calendar", std::array<DesktopEntry, 1>{calculator}));
    const auto editor = issueEntry("com.github.example.Editor", "Editor", "editor");
    TEST_CHECK(!app_identity::findDesktopEntry("com.github.other.Player", std::array<DesktopEntry, 1>{editor}));
  }

  void testAppImageOriginDetection() {
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / ("noctalia-appimage-origin-" + std::to_string(getpid()));
    const fs::path applications = root / "data/applications";
    const fs::path systemApplications = root / "system/applications";
    const fs::path executable = root / "PortableApp";
    const fs::path bin = root / "bin";
    const fs::path workingDirectory = root / "cwd";
    const fs::path inaccessibleApplications = applications / "inaccessible";
    fs::create_directories(applications);
    fs::create_directories(systemApplications);
    fs::create_directories(bin);
    fs::create_directories(workingDirectory);
    fs::create_directories(inaccessibleApplications);
    {
      std::ofstream binary(executable, std::ios::binary);
      binary.write(
          "\x7F"
          "ELF\x02\x01\x01\0"
          "AI",
          10
      );
      chmod(executable.c_str(), 0700);
    }
    {
      std::ofstream entry(applications / "metadata.desktop");
      entry
          << "[Desktop Entry]\nType=Application\nName=Metadata AppImage\nExec=metadata-app\n"
          << "X-AppImage-Version=1.0\n";
    }
    {
      std::ofstream entry(applications / "portable.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Portable AppImage\nExec=" << executable.string() << "\n";
    }
    {
      std::ofstream entry(applications / "suffix.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Suffixed AppImage\nExec=/opt/Suffixed.AppImage\n";
    }
    {
      std::ofstream entry(applications / "env-wrapped.desktop");
      entry
          << "[Desktop Entry]\nType=Application\nName=Env Wrapped AppImage\n"
          << "Exec=env DESKTOPINTEGRATION=1 \"FOO=a b\" "
          << executable.string()
          << " %U\n";
    }
    {
      std::ofstream entry(applications / "try-exec.desktop");
      entry
          << "[Desktop Entry]\nType=Application\nName=TryExec AppImage\nTryExec="
          << executable.string()
          << "\nExec=sh -c launch\n";
    }
    {
      std::ofstream binary(bin / "native-app");
      binary << "#!/bin/sh\nexit 0\n";
      chmod((bin / "native-app").c_str(), 0700);
      fs::create_symlink(executable, bin / "path-appimage");
      fs::create_symlink(executable, bin / "shadowed-native");
      fs::create_symlink(executable, workingDirectory / "native-app");
    }
    {
      std::ofstream entry(applications / "native.desktop");
      entry
          << "[Desktop Entry]\nType=Application\nName=Native App\nExec=native-app\n"
          << "X-AppImage-Homepage=https://example.com/native-app\n";
    }
    {
      std::ofstream entry(applications / "appimage-opt-out.desktop");
      entry
          << "[Desktop Entry]\nType=Application\nName=AppImage Opt Out\nExec=native-app\n"
          << "X-AppImage-Integrate=false\n";
    }
    {
      std::ofstream entry(applications / "path.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=PATH AppImage\nExec=path-appimage\n";
    }
    {
      std::ofstream entry(systemApplications / "shadowed-native.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Shadowed Native App\nExec=shadowed-native\n";
    }
    {
      std::ofstream entry(inaccessibleApplications / "inaccessible.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Inaccessible App\nExec=inaccessible-app\n";
    }
    {
      std::ofstream entry(applications / "userapp-Firefox-ABCD.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Firefox\nNoDisplay=true\nExec=firefox\n";
    }
    {
      std::ofstream entry(systemApplications / "firefox.desktop");
      entry << "[Desktop Entry]\nType=Application\nName=Firefox\nExec=firefox\nIcon=firefox\n";
    }
    chmod(inaccessibleApplications.c_str(), 0000);

    const char* oldDataHome = std::getenv("XDG_DATA_HOME");
    const char* oldDataDirs = std::getenv("XDG_DATA_DIRS");
    const char* oldPath = std::getenv("PATH");
    const std::optional<std::string> savedDataHome =
        oldDataHome == nullptr ? std::nullopt : std::optional<std::string>(oldDataHome);
    const std::optional<std::string> savedDataDirs =
        oldDataDirs == nullptr ? std::nullopt : std::optional<std::string>(oldDataDirs);
    const std::optional<std::string> savedPath =
        oldPath == nullptr ? std::nullopt : std::optional<std::string>(oldPath);
    const fs::path savedWorkingDirectory = fs::current_path();
    setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    setenv("XDG_DATA_DIRS", (root / "system").c_str(), 1);
    setenv("PATH", bin.c_str(), 1);
    fs::current_path(workingDirectory);

    const auto entries = scanDesktopEntries();
    const auto findOrigin = [&](std::string_view id) {
      const auto it = std::ranges::find(entries, id, &DesktopEntry::id);
      return it == entries.end() ? DesktopEntryOrigin::Unknown : it->origin;
    };
    const DesktopEntryOrigin metadataOrigin = findOrigin("metadata");
    const DesktopEntryOrigin portableOrigin = findOrigin("portable");
    const DesktopEntryOrigin suffixOrigin = findOrigin("suffix");
    const DesktopEntryOrigin envWrappedOrigin = findOrigin("env-wrapped");
    const DesktopEntryOrigin tryExecOrigin = findOrigin("try-exec");
    const DesktopEntryOrigin nativeOrigin = findOrigin("native");
    const DesktopEntryOrigin optOutOrigin = findOrigin("appimage-opt-out");
    const DesktopEntryOrigin pathOrigin = findOrigin("path");
    const DesktopEntryOrigin shadowedNativeOrigin = findOrigin("shadowed-native");
    const bool userappHidden =
        std::ranges::none_of(entries, [](const DesktopEntry& entry) { return entry.id == "userapp-Firefox-ABCD"; });
    const DesktopEntry firefox = app_identity::resolveRunningDesktopEntry("firefox", entries);

    chmod(inaccessibleApplications.c_str(), 0700);
    fs::current_path(savedWorkingDirectory);

    if (savedDataHome.has_value()) {
      setenv("XDG_DATA_HOME", savedDataHome->c_str(), 1);
    } else {
      unsetenv("XDG_DATA_HOME");
    }
    if (savedDataDirs.has_value()) {
      setenv("XDG_DATA_DIRS", savedDataDirs->c_str(), 1);
    } else {
      unsetenv("XDG_DATA_DIRS");
    }
    if (savedPath.has_value()) {
      setenv("PATH", savedPath->c_str(), 1);
    } else {
      unsetenv("PATH");
    }
    fs::remove_all(root);

    TEST_CHECK(metadataOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(portableOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(suffixOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(envWrappedOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(tryExecOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(nativeOrigin == DesktopEntryOrigin::System);
    TEST_CHECK(optOutOrigin == DesktopEntryOrigin::System);
    TEST_CHECK(pathOrigin == DesktopEntryOrigin::AppImage);
    TEST_CHECK(shadowedNativeOrigin == DesktopEntryOrigin::System);
    TEST_CHECK(userappHidden);
    TEST_CHECK(firefox.id == "firefox");
    TEST_CHECK(firefox.icon == "firefox");
  }

} // namespace

int main() {
  const DesktopEntry chat = sampleChatEntry();

  expectMatch(chat, "sample-chat-desktop");
  expectMatch(chat, "samplechat");
  expectMatch(chat, "sample chat");
  expectMatch(chat, "sample.chat.desktop");
  expectMatch(chat, "sample_chat_desktop");
  expectMatch(chat, "sample chat desktop");
  expectMatch(chat, "Sample.ChatDesktop");
  expectNoMatch(chat, "");
  expectNoMatch(chat, "sample-calendar");

  const DesktopEntry displayNameOnly = displayNameOnlyEntry();
  expectMatch(displayNameOnly, "risky match");
  expectNoMatch(displayNameOnly, "risky.match");

  const std::vector<DesktopEntry> entries = {chat};
  const DesktopEntry resolved = app_identity::resolveRunningDesktopEntry("Sample.ChatDesktop", entries);

  TEST_CHECK(resolved.id == "sample-chat-desktop");
  TEST_CHECK(resolved.exec == "sample-chat-desktop");
  TEST_CHECK(resolved.icon == "sample-chat-desktop");

  const DesktopEntry fallback = app_identity::resolveRunningDesktopEntry("Unknown.App", entries);
  TEST_CHECK(fallback.id == "Unknown.App");
  TEST_CHECK(fallback.name == "Unknown.App");
  TEST_CHECK(fallback.nameLower == "unknown.app");
  TEST_CHECK(fallback.exec.empty());
  TEST_CHECK(fallback.icon.empty());

  // Hidden/NoDisplay entries are excluded at parse time, so the resolver never receives one in
  // production and does not re-filter them. If one is present it resolves like any other entry.
  DesktopEntry hidden = sampleChatEntry();
  hidden.hidden = true;
  TEST_CHECK(
      app_identity::resolveRunningDesktopEntry("Sample.ChatDesktop", std::array<DesktopEntry, 1>{hidden}).id
      == "sample-chat-desktop"
  );

  DesktopEntry noDisplay = sampleChatEntry();
  noDisplay.noDisplay = true;
  TEST_CHECK(
      app_identity::resolveRunningDesktopEntry("Sample.ChatDesktop", std::array<DesktopEntry, 1>{noDisplay}).id
      == "sample-chat-desktop"
  );

  const std::vector<DesktopEntry> multipleEntries = {sampleChatEntry(), sampleMailEntry()};
  const auto resolvedApps = app_identity::resolveRunningApps(
      std::array<std::string, 3>{"Sample.ChatDesktop", "sample-chat-desktop", "SampleMail"}, multipleEntries
  );
  TEST_CHECK(resolvedApps.size() == 2);
  TEST_CHECK(resolvedApps[0].entry.id == "sample-chat-desktop");
  TEST_CHECK(resolvedApps[1].entry.id == "sample-mail");

  const auto unknownApps =
      app_identity::resolveRunningApps(std::array<std::string, 2>{"Unknown.App", "unknown-app"}, multipleEntries);
  TEST_CHECK(unknownApps.size() == 2);
  TEST_CHECK(unknownApps[0].entry.id == "Unknown.App");
  TEST_CHECK(unknownApps[1].entry.id == "unknown-app");

  const DesktopEntry easyEffects = easyEffectsEntry();
  const DesktopEntry kdeResolved =
      app_identity::resolveRunningDesktopEntry("org.kde.easyeffects", std::array<DesktopEntry, 1>{easyEffects});
  TEST_CHECK(kdeResolved.id == "com.github.wwmm.easyeffects");
  TEST_CHECK(kdeResolved.name == "Easy Effects");
  TEST_CHECK(kdeResolved.icon == "easyeffects");

  const std::vector<DesktopEntry> ambiguousTail = {
      duplicateTailEntry("com.foo.easyeffects", "foo-easyeffects"),
      duplicateTailEntry("com.bar.easyeffects", "bar-easyeffects"),
  };
  const DesktopEntry ambiguousResolved = app_identity::resolveRunningDesktopEntry("org.kde.easyeffects", ambiguousTail);
  TEST_CHECK(ambiguousResolved.id == "org.kde.easyeffects");

  const DesktopEntry iconlessFirefox = issueEntry("userapp-Firefox-ABCD", "Firefox", "");
  const DesktopEntry canonicalFirefox = issueEntry("firefox", "Firefox", "firefox");
  const DesktopEntry rankedFirefox = app_identity::resolveRunningDesktopEntry(
      "firefox", std::array<DesktopEntry, 2>{iconlessFirefox, canonicalFirefox}
  );
  TEST_CHECK(rankedFirefox.id == "firefox");
  TEST_CHECK(rankedFirefox.icon == "firefox");

  const DesktopEntry threema = issueEntry("ch.threema.threema-desktop", "Threema Beta", "ch.threema.threema-desktop");
  const DesktopEntry eduVpn = issueEntry("nl.eduvpn.org.eduvpn", "eduVPN", "nl.eduvpn.org.eduvpn");
  const auto issueApps = app_identity::resolveRunningApps(
      std::array<std::string, 2>{"Threema", ".eduvpn-gui-wrapped"}, std::array<DesktopEntry, 2>{threema, eduVpn}
  );
  TEST_CHECK(issueApps.size() == 2);
  TEST_CHECK(issueApps[0].entry.id == "ch.threema.threema-desktop");
  TEST_CHECK(issueApps[0].entry.icon == "ch.threema.threema-desktop");
  TEST_CHECK(issueApps[1].entry.id == "nl.eduvpn.org.eduvpn");
  TEST_CHECK(issueApps[1].entry.icon == "nl.eduvpn.org.eduvpn");

  const auto ambiguousWrapper = std::array<DesktopEntry, 2>{
      issueEntry("org.alpha.eduvpn", "Alpha VPN", "alpha-eduvpn"),
      issueEntry("org.beta.eduvpn", "Beta VPN", "beta-eduvpn"),
  };
  const DesktopEntry unresolvedWrapper =
      app_identity::resolveRunningDesktopEntry(".eduvpn-gui-wrapped", ambiguousWrapper);
  TEST_CHECK(unresolvedWrapper.id == ".eduvpn-gui-wrapped");
  TEST_CHECK(unresolvedWrapper.icon.empty());

  DesktopEntry steam = issueEntry("steam", "Steam", "steam");
  DesktopEntry steamGame = issueEntry("terraria", "Terraria", "terraria");
  steamGame.exec = "steam steam://rungameid/105600";
  const DesktopEntry resolvedSteamGame =
      app_identity::resolveRunningDesktopEntry("steam_app_105600", std::array<DesktopEntry, 2>{steam, steamGame});
  TEST_CHECK(resolvedSteamGame.id == "terraria");
  TEST_CHECK(resolvedSteamGame.startupWmClass == "steam_app_105600");

  testSharedNamespaceDoesNotIdentifyApplication();
  testAppImageOriginDetection();

  return 0;
}
