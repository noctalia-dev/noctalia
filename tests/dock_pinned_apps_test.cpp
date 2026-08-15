#include "shell/dock/pinned_apps.h"
#include "system/internal_app_metadata.h"
#include "tests/test_check.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace internal_apps {

  std::optional<AppMetadata> metadataForAppId(std::string_view /*appId*/) { return std::nullopt; }

  void applyMetadataToDesktopEntry(DesktopEntry& /*entry*/) {}

} // namespace internal_apps

namespace {

  DesktopEntry sampleEntry() {
    DesktopEntry entry;
    entry.id = "sample-chat.desktop";
    entry.name = "Sample Chat";
    entry.nameLower = "sample chat";
    entry.startupWmClass = "SampleChat";
    entry.startupWmClassLower = "samplechat";
    entry.exec = "sample-chat";
    entry.icon = "sample-chat";
    return entry;
  }

  DesktopEntry pathStyleEntry() {
    DesktopEntry entry;
    entry.id = "/usr/share/applications/org.example.Mail.desktop";
    entry.name = "Example Mail";
    entry.nameLower = "example mail";
    entry.startupWmClass = "ExampleMail";
    entry.startupWmClassLower = "examplemail";
    return entry;
  }

} // namespace

int main() {
  const DesktopEntry chat = sampleEntry();

  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(chat, "sample-chat.desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "SampleChat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample chat"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "sample_chat_desktop"));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, ""));
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(chat, "calendar"));

  const DesktopEntry mail = pathStyleEntry();
  TEST_CHECK(!shell::dock::pinned_apps::matchesEntry(mail, "org.example.Mail"));
  TEST_CHECK(shell::dock::pinned_apps::matchesEntry(mail, "/usr/share/applications/org.example.Mail.desktop"));

  std::vector<std::string> pinned = {"calendar", "samplechat", "sample-chat.desktop"};
  TEST_CHECK(shell::dock::pinned_apps::containsEntry(pinned, chat));

  shell::dock::pinned_apps::removeEntry(pinned, chat);
  TEST_CHECK((pinned == std::vector<std::string>{"calendar", "samplechat"}));

  return 0;
}
