#include "dbus/notification/notification_service.h"
#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "tests/notification_history_test_utils.h"
#include "tests/test_check.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <unistd.h>

int main() {
  using notification_dbus::notifyIconIsAppIcon;
  TEST_CHECK(notifyIconIsAppIcon("org.example.App", {}));
  TEST_CHECK(notifyIconIsAppIcon("/usr/share/icons/example.png", {}));
  TEST_CHECK(!notifyIconIsAppIcon("", {}));
  TEST_CHECK(!notifyIconIsAppIcon("https://example.invalid/avatar.png", {}));
  for (const auto* hint : {"image-path", "image_path", "image-data", "image_data", "icon_data"}) {
    const std::map<std::string, sdbus::Variant> hints{{hint, sdbus::Variant(std::string("image"))}};
    TEST_CHECK(!notifyIconIsAppIcon("org.example.App", hints));
  }

  NotificationManager manager;
  const auto id = notification_dbus::ingestNotify(manager, "App", 0, "org.example.App", "Message", "", {}, {}, 0);
  TEST_CHECK(manager.findHistoryEntry(id) != nullptr);
  TEST_CHECK(manager.findHistoryEntry(id)->notification.iconIsAppIcon);

  char directory[] = "/tmp/noctalia-icon-source-XXXXXX";
  TEST_CHECK(mkdtemp(directory) != nullptr);
  const auto path = std::filesystem::path(directory) / "history.enc";
  const auto key = notificationHistoryTestKey();
  TEST_CHECK(saveNotificationHistoryToFile(path, manager.history(), id + 1, 1, key));
  std::deque<NotificationHistoryEntry> restored;
  std::uint32_t nextId = 0;
  std::uint64_t serial = 0;
  TEST_CHECK(loadNotificationHistoryFromFile(path, restored, nextId, serial, key));
  TEST_CHECK(restored.size() == 1);
  TEST_CHECK(restored.front().notification.iconIsAppIcon);

  auto legacy = readNotificationHistoryJson(path, key);
  legacy["entries"][0]["notification"].erase("icon_is_app_icon");
  const std::string legacyText = legacy.dump();
  const auto* legacyBytes = reinterpret_cast<const std::uint8_t*>(legacyText.data());
  TEST_CHECK(
      security::writeEncryptedFile(path, {legacyBytes, legacyText.size()}, key, {"notification-history-v1", "history"})
  );
  TEST_CHECK(loadNotificationHistoryFromFile(path, restored, nextId, serial, key));
  TEST_CHECK(!restored.front().notification.iconIsAppIcon);

  const std::map<std::string, sdbus::Variant> imageHints{
      {"image-path", sdbus::Variant(std::string("org.example.App"))},
  };
  TEST_CHECK(
      notification_dbus::ingestNotify(manager, "App", id, "org.example.App", "Message", "", {}, imageHints, 0) == id
  );
  TEST_CHECK(!manager.findHistoryEntry(id)->notification.iconIsAppIcon);
  TEST_CHECK(saveNotificationHistoryToFile(path, manager.history(), id + 1, 2, key));
  TEST_CHECK(loadNotificationHistoryFromFile(path, restored, nextId, serial, key));
  TEST_CHECK(!restored.front().notification.iconIsAppIcon);
  std::filesystem::remove_all(directory);
}
