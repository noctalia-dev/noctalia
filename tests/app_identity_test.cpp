#include "system/app_identity.h"
#include "system/internal_app_metadata.h"
#include "util/string_utils.h"

#include <cassert>
#include <string>
#include <optional>
#include <string_view>
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
    assert(app_identity::desktopEntryMatchesLower(entry, token));
  }

  void expectNoMatch(const DesktopEntry& entry, std::string_view token) {
    assert(!app_identity::desktopEntryMatchesLower(entry, token));
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

  assert(resolved.id == "sample-chat-desktop");
  assert(resolved.exec == "sample-chat-desktop");
  assert(resolved.icon == "sample-chat-desktop");

  const DesktopEntry fallback = app_identity::resolveRunningDesktopEntry("Unknown.App", entries);
  assert(fallback.id == "Unknown.App");
  assert(fallback.name == "Unknown.App");
  assert(fallback.nameLower == "unknown.app");
  assert(fallback.exec.empty());
  assert(fallback.icon.empty());

  DesktopEntry hidden = sampleChatEntry();
  hidden.hidden = true;
  assert(app_identity::resolveRunningDesktopEntry("Sample.ChatDesktop", {hidden}).id == "Sample.ChatDesktop");

  DesktopEntry noDisplay = sampleChatEntry();
  noDisplay.noDisplay = true;
  assert(app_identity::resolveRunningDesktopEntry("Sample.ChatDesktop", {noDisplay}).id == "Sample.ChatDesktop");

  const std::vector<DesktopEntry> multipleEntries = {sampleChatEntry(), sampleMailEntry()};
  const auto resolvedApps =
      app_identity::resolveRunningApps({"Sample.ChatDesktop", "sample-chat-desktop", "SampleMail"}, multipleEntries);
  assert(resolvedApps.size() == 2);
  assert(resolvedApps[0].entry.id == "sample-chat-desktop");
  assert(resolvedApps[1].entry.id == "sample-mail");

  const auto unknownApps = app_identity::resolveRunningApps({"Unknown.App", "unknown-app"}, multipleEntries);
  assert(unknownApps.size() == 2);
  assert(unknownApps[0].entry.id == "Unknown.App");
  assert(unknownApps[1].entry.id == "unknown-app");

  const DesktopEntry easyEffects = easyEffectsEntry();
  const DesktopEntry kdeResolved =
      app_identity::resolveRunningDesktopEntry("org.kde.easyeffects", {easyEffects});
  assert(kdeResolved.id == "com.github.wwmm.easyeffects");
  assert(kdeResolved.name == "Easy Effects");
  assert(kdeResolved.icon == "easyeffects");

  const std::vector<DesktopEntry> ambiguousTail = {
      duplicateTailEntry("com.foo.easyeffects", "foo-easyeffects"),
      duplicateTailEntry("com.bar.easyeffects", "bar-easyeffects"),
  };
  const DesktopEntry ambiguousResolved =
      app_identity::resolveRunningDesktopEntry("org.kde.easyeffects", ambiguousTail);
  assert(ambiguousResolved.id == "org.kde.easyeffects");

  return 0;
}
