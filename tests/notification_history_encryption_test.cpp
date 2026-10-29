#include "core/deferred_call.h"
#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "notification_history_test_utils.h"
#include "util/file_utils.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  const security::EncryptionContext kHistoryContext{"notification-history-v1", "history"};

  struct TestDirectory {
    fs::path path = fs::temp_directory_path() / ("noctalia-notification-encryption-" + std::to_string(getpid()));
    TestDirectory() {
      fs::remove_all(path);
      fs::create_directories(path);
    }
    ~TestDirectory() { fs::remove_all(path); }
  };

  std::string contents(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    TEST_CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
    TEST_CHECK(output.good());
  }

  void drain() {
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
  }

  void privateFile(const fs::path& path) {
    TEST_CHECK((fs::status(path).permissions() & fs::perms::mask) == static_cast<fs::perms>(0600));
  }

  void testEncryption(const fs::path& dir) {
    const auto key = notificationHistoryTestKey();
    const auto wrongKey = notificationHistoryTestKey(0x43);
    const auto path = dir / "history.enc";
    std::deque<NotificationHistoryEntry> entries{{
        .notification =
            {
                .id = 7,
                .summary = "Bank",
                .body = "Your code is 482913",
                .imageData =
                    NotificationImageData{
                        .width = 2, .height = 2, .rowStride = 8, .data = std::vector<std::uint8_t>(16, 128)
                    },
            },
        .eventSerial = 9,
    }};
    TEST_CHECK(saveNotificationHistoryToFile(path, entries, 8, 9, key));
    privateFile(path);
    const auto ciphertext = contents(path);
    TEST_CHECK(ciphertext.starts_with("NOCTALIAENC1"));
    TEST_CHECK(!ciphertext.contains("482913"));
    const auto manifest = readNotificationHistoryJson(path, key);
    const auto filename = manifest["entries"][0]["notification"]["image_data"]["image_file"].get<std::string>();
    const auto asset = dir / "notification_history_assets" / filename;
    TEST_CHECK(filename.ends_with(".webp.enc"));
    privateFile(asset);
    TEST_CHECK((fs::status(asset.parent_path()).permissions() & fs::perms::mask) == static_cast<fs::perms>(0700));
    const auto imageCiphertext = contents(asset);
    TEST_CHECK(imageCiphertext.starts_with("NOCTALIAENC1"));
    TEST_CHECK(!imageCiphertext.contains("WEBP"));
    TEST_CHECK(!security::readEncryptedFile(asset, key, kHistoryContext, 1024).succeeded());
    TEST_CHECK(!security::readEncryptedFile(asset, key, {"notification-image-v1", "other.webp.enc"}, 1024).succeeded());

    std::deque<NotificationHistoryEntry> loaded;
    std::uint32_t nextId = 0;
    std::uint64_t serial = 0;
    TEST_CHECK(loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(loaded.size() == 1);
    TEST_CHECK(loaded.front().notification.body == "Your code is 482913");
    TEST_CHECK(loaded.front().notification.imageData->data.size() == 16);
    TEST_CHECK(nextId == 8);
    TEST_CHECK(serial == 9);
    TEST_CHECK(!loadNotificationHistoryFromFile(path, loaded, nextId, serial, wrongKey));
    TEST_CHECK(contents(path) == ciphertext);
    TEST_CHECK(loaded.size() == 1);

    // A corrupt encrypted file must never fall back to a stale plaintext manifest.
    write(dir / "history.json", R"({"entries":[],"next_id":1})");
    auto damaged = ciphertext;
    damaged.back() ^= 1;
    write(path, damaged);
    TEST_CHECK(!loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(contents(path) == damaged);
    TEST_CHECK(fs::exists(dir / "history.json"));
    write(path, ciphertext);
    // A damaged preview drops only that image; the authenticated manifest still loads.
    write(asset, "damaged image");
    TEST_CHECK(loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(loaded.size() == 1);
    TEST_CHECK(loaded.front().notification.body == "Your code is 482913");
    TEST_CHECK(!loaded.front().notification.imageData.has_value());
    TEST_CHECK(!fs::exists(dir / "history.json"));
    TEST_CHECK(contents(path) == ciphertext);
    write(asset, imageCiphertext);
    TEST_CHECK(loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(loaded.front().notification.imageData->data.size() == 16);

    write(asset.parent_path() / "n_123.rgba", "old plaintext pixels");
    TEST_CHECK(saveNotificationHistoryToFile(path, {}, 8, 9, key));
    TEST_CHECK(fs::is_empty(asset.parent_path()));
  }

  void testMigration(const fs::path& dir) {
    const auto key = notificationHistoryTestKey();
    const auto path = dir / "history.enc";
    const auto legacy = dir / "history.json";
    const auto assets = dir / "notification_history_assets";
    write(assets / "n_1.rgba", std::string(16, '\x7f'));
    write(assets / "i_orphan.webp", "orphan plaintext image");
    write(
        legacy,
        R"({"version":2,"next_id":2,"change_serial":3,"entries":[{"notification":{"id":1,"body":"legacy secret","image_data":{"width":2,"height":2,"row_stride":8,"channels":4,"image_file":"n_1.rgba"}},"event_serial":3}]})"
    );
    write(legacy.string() + ".tmp", "abandoned plaintext write");
    fs::permissions(legacy.string() + ".tmp", static_cast<fs::perms>(0644));
    fs::permissions(legacy, static_cast<fs::perms>(0644));
    fs::permissions(assets, static_cast<fs::perms>(0755));
    TEST_CHECK(secureNotificationHistory(path));
    privateFile(legacy);
    privateFile(legacy.string() + ".tmp");
    TEST_CHECK((fs::status(assets).permissions() & fs::perms::mask) == static_cast<fs::perms>(0700));
    std::deque<NotificationHistoryEntry> loaded;
    std::uint32_t nextId = 0;
    std::uint64_t serial = 0;

    // An atomic-write failure preserves the source manifest and every old sidecar.
    fs::create_directory(path.string() + ".tmp");
    const auto originalJson = contents(legacy);
    const auto originalPixels = contents(assets / "n_1.rgba");
    TEST_CHECK(!loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(!fs::exists(path));
    TEST_CHECK(contents(legacy) == originalJson);
    TEST_CHECK(contents(assets / "n_1.rgba") == originalPixels);
    TEST_CHECK(contents(legacy.string() + ".tmp") == "abandoned plaintext write");
    fs::remove(path.string() + ".tmp");
    TEST_CHECK(loadNotificationHistoryFromFile(path, loaded, nextId, serial, key));
    TEST_CHECK(loaded.size() == 1);
    TEST_CHECK(loaded.front().notification.body == "legacy secret");
    TEST_CHECK(loaded.front().notification.imageData->data.size() == 16);
    TEST_CHECK(!fs::exists(legacy));
    TEST_CHECK(!fs::exists(legacy.string() + ".tmp"));
    TEST_CHECK(!fs::exists(assets / "n_1.rgba"));
    TEST_CHECK(!fs::exists(assets / "i_orphan.webp"));
    for (const auto& item : fs::directory_iterator(assets)) {
      TEST_CHECK(contents(item.path()).starts_with("NOCTALIAENC1"));
      privateFile(item.path());
    }

    write(dir / "orphan.json.tmp", "uncommitted plaintext history");
    TEST_CHECK(!loadNotificationHistoryFromFile(dir / "orphan.enc", loaded, nextId, serial, key));
    TEST_CHECK(contents(dir / "orphan.json.tmp") == "uncommitted plaintext history");
    privateFile(dir / "orphan.json.tmp");

    const auto invalid = dir / "invalid.enc";
    write(dir / "invalid.json", R"({"entries":[{"notification":{"id":"invalid"}}]})");
    TEST_CHECK(!loadNotificationHistoryFromFile(invalid, loaded, nextId, serial, key));
    TEST_CHECK(fs::exists(dir / "invalid.json"));
    TEST_CHECK(!fs::exists(invalid));
    write(dir / "invalid.json", R"({"entries":[{"notification":{"image_data":{"image_file":"../outside"}}}]})");
    TEST_CHECK(!loadNotificationHistoryFromFile(invalid, loaded, nextId, serial, key));
    write(dir / "invalid.json", std::string(16 * 1024 * 1024 + 1, 'x'));
    TEST_CHECK(!loadNotificationHistoryFromFile(invalid, loaded, nextId, serial, key));

    // A missing legacy preview must not block migration and leave plaintext behind.
    const auto missing = dir / "missing.enc";
    write(
        dir / "missing.json",
        R"({"version":2,"entries":[{"notification":{"id":1,"body":"legacy secret","image_data":{"width":2,"height":2,"channels":4,"image_file":"n_404.rgba"}}}]})"
    );
    TEST_CHECK(loadNotificationHistoryFromFile(missing, loaded, nextId, serial, key));
    TEST_CHECK(loaded.size() == 1);
    TEST_CHECK(!loaded.front().notification.imageData.has_value());
    TEST_CHECK(!fs::exists(dir / "missing.json"));
    TEST_CHECK(fs::exists(missing));
  }

  void testManager(const fs::path& dir) {
    TEST_CHECK(setenv("NOCTALIA_STATE_HOME", dir.c_str(), 1) == 0);
    const auto path = fs::path(FileUtils::stateDir()) / "notification_history.enc";
    const auto keyFile = dir / "storage-key";
    write(keyFile, std::string(64, '1'));
    security::SecretStore store;
    security::StorageKeyProvider provider(store);
    provider.configure(StorageKeySource::File, keyFile.string(), false);
    TEST_CHECK(provider.state() == security::StorageKeyState::Ready);
    const auto key = provider.deriveKey(security::StorageKeyPurpose::NotificationHistory);
    const auto clipboardKey = provider.deriveKey(security::StorageKeyPurpose::ClipboardHistory);
    const auto calendarKey = provider.deriveKey(security::StorageKeyPurpose::CalendarEvents);
    TEST_CHECK(key);
    TEST_CHECK(clipboardKey);
    TEST_CHECK(calendarKey);
    const std::deque<NotificationHistoryEntry> saved{
        {.notification = {
             .id = 1, .origin = NotificationOrigin::External, .body = "saved secret", .urgency = Urgency::Normal
         }}
    };
    TEST_CHECK(saveNotificationHistoryToFile(path, saved, 2, 1, *key));
    TEST_CHECK(!security::readEncryptedFile(path, *clipboardKey, kHistoryContext, 4096).succeeded());
    TEST_CHECK(!security::readEncryptedFile(path, *calendarKey, kHistoryContext, 4096).succeeded());
    const auto before = contents(path);

    NotificationManager manager;
    manager.loadPersistedHistory();
    TEST_CHECK(manager.history().empty());
    const auto liveId = manager.addOrReplace({.appName = "test", .body = "session secret", .timeout = 0});
    manager.flushPersistedHistory();
    TEST_CHECK(contents(path) == before);
    TEST_CHECK(!fs::exists(path.parent_path() / "notification_history.json"));
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(manager.history().size() == 2);
    TEST_CHECK(!manager.historyNeedsRecovery());
    TEST_CHECK(manager.history().front().notification.id != liveId);
    TEST_CHECK(manager.all().front().id == liveId);
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(manager.history().size() == 2);
    drain();
    const auto good = contents(path);
    write(keyFile, std::string(64, '2'));
    provider.retry();
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(manager.historyNeedsRecovery());
    manager.addOrReplace({.appName = "test", .body = "new session secret", .timeout = 0});
    manager.flushPersistedHistory();
    TEST_CHECK(contents(path) == good);
    write(keyFile, std::string(64, '1'));
    provider.retry();
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(!manager.historyNeedsRecovery());
    TEST_CHECK(manager.history().size() == 3);
    drain();

    fs::remove(keyFile);
    provider.retry();
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(manager.historyNeedsRecovery());
    const auto locked = contents(path);
    manager.flushPersistedHistory();
    TEST_CHECK(contents(path) == locked);
    TEST_CHECK(manager.hasEncryptedHistory());
    TEST_CHECK(manager.clearEncryptedHistoryForRecovery());
    TEST_CHECK(!fs::exists(path));
    TEST_CHECK(!manager.hasEncryptedHistory());
    TEST_CHECK(!manager.historyNeedsRecovery());
    TEST_CHECK(manager.history().empty());
    write(keyFile, std::string(64, '1'));
    provider.setChangeCallback([&]() { manager.syncHistoryPersistence(provider); });
    bool reset = false;
    provider.resetAfterEncryptedDataCleared([&](bool success) { reset = success; });
    TEST_CHECK(reset);
    TEST_CHECK(manager.historyPersistenceReady());
    TEST_CHECK(contents(keyFile) == std::string(64, '1'));
    TEST_CHECK(readNotificationHistoryJson(path, *key)["entries"].empty());
    TEST_CHECK(!fs::exists(path.parent_path() / "notification_history_assets"));
    drain();
    TEST_CHECK(readNotificationHistoryJson(path, *key)["entries"].empty());
    provider.setChangeCallback({});
  }
  void testClearBeforeUnlock(const fs::path& dir) {
    TEST_CHECK(setenv("NOCTALIA_STATE_HOME", dir.c_str(), 1) == 0);
    const auto path = fs::path(FileUtils::stateDir()) / "notification_history.enc";
    const auto keyFile = dir / "storage-key";
    write(keyFile, std::string(64, '1'));
    security::SecretStore store;
    security::StorageKeyProvider provider(store);
    provider.configure(StorageKeySource::File, keyFile.string(), false);
    const auto key = provider.deriveKey(security::StorageKeyPurpose::NotificationHistory);
    TEST_CHECK(key.has_value());
    const std::deque<NotificationHistoryEntry> saved{
        {.notification = {
             .id = 1, .origin = NotificationOrigin::External, .body = "cleared secret", .urgency = Urgency::Normal
         }}
    };
    TEST_CHECK(saveNotificationHistoryToFile(path, saved, 2, 1, *key));
    NotificationManager manager;
    manager.clearHistory();
    manager.syncHistoryPersistence(provider);
    TEST_CHECK(manager.history().empty());
    TEST_CHECK(readNotificationHistoryJson(path, *key)["entries"].empty());
    drain();
  }

} // namespace

int main() {
  const TestDirectory root;
  const auto previousUmask = umask(0022);
  testEncryption(root.path / "encryption");
  testMigration(root.path / "migration");
  testManager(root.path / "state");
  testClearBeforeUnlock(root.path / "cleared");
  umask(previousUmask);
  return 0;
}
