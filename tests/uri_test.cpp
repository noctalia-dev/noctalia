#include "dbus/mpris/mpris_art.h"
#include "dbus/notification/notification_service.h"
#include "net/uri.h"
#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "render/core/image_encoder.h"
#include "render/core/image_file_loader.h"
#include "tests/test_check.h"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

  struct TempDirectory {
    std::filesystem::path path;

    TempDirectory() {
      std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-uri-consumers-XXXXXX").string();
      const char* created = mkdtemp(pattern.data());
      TEST_CHECK(created != nullptr);
      path = created;
    }

    ~TempDirectory() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };

  void writePixel(const std::filesystem::path& path, const std::vector<std::uint8_t>& pixel) {
    const auto png = encodePng(pixel.data(), 1, 1);
    TEST_CHECK(!png.empty());
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    file.close();
    TEST_CHECK(file.good());
  }

  bool check(bool condition, const std::string& context) {
    if (!condition) {
      std::println(stderr, "uri_test: {}", context);
    }
    return condition;
  }

  bool expectPath(std::string_view input, std::string_view expected) {
    const auto actual = uri::normalizeFileUrl(input);
    return check(actual == expected, "unexpected normalized path for " + std::string(input));
  }

} // namespace

int main() {
  TempDirectory temp;
  const auto literal = temp.path / "icon%20name.png";
  const auto space = temp.path / "icon name.png";
  const std::vector<std::uint8_t> green{0, 255, 0, 255};
  const std::vector<std::uint8_t> red{255, 0, 0, 255};
  writePixel(literal, green);
  writePixel(space, red);

  struct Case {
    std::string input;
    std::vector<std::uint8_t> expected;
  };
  const std::vector<Case> cases{
      {literal.string(), green},
      {space.string(), red},
      {"file://" + (temp.path / "icon%2520name.png").string(), green},
      {"file://" + (temp.path / "icon%20name.png").string(), red},
  };

  bool ok = true;
  // Keep cases that do not require file loading, including icon names and
  // unsupported inputs. Native paths and file URIs are exercised below.
  ok = expectPath("/tmp/icon%2Fname.png", "/tmp/icon%2Fname.png") && ok;
  ok = expectPath("/tmp/icon%00name.png", "/tmp/icon%00name.png") && ok;
  ok = expectPath("/tmp/100% ready.png", "/tmp/100% ready.png") && ok;
  ok = expectPath("icon%20name", "icon%20name") && ok;
  ok = expectPath("file://localhost/tmp/icon%20name.png", "/tmp/icon name.png") && ok;
  ok = expectPath("file:///tmp/icon%2bname.png", "/tmp/icon+name.png") && ok;
  ok = expectPath("", "") && ok;
  ok = expectPath("http://example.com/icon.png", "") && ok;
  ok = expectPath("https://example.com/icon.png", "") && ok;

  NotificationManager manager;
  std::map<std::uint32_t, std::vector<std::uint8_t>> expectedSnapshots;
  for (const auto& test : cases) {
    // Resolve artwork through the consumer and decode the selected file, so a
    // different existing filename cannot silently satisfy the test.
    std::unordered_set<std::string> pending;
    const auto artPath = mpris::resolveArtworkSource(nullptr, pending, test.input, []() {}, {});
    const auto image = loadImageFile(artPath);
    ok = check(
             image && image->width == 1 && image->height == 1 && image->rgba == test.expected,
             "wrong MPRIS artwork pixels for " + test.input
         )
        && ok;

    for (const char* key : {"image-path", "image_path"}) {
      const std::map<std::string, sdbus::Variant> hints{{key, sdbus::Variant(test.input)}};
      const auto id = notification_dbus::ingestNotify(manager, "uri-test", 0, "", test.input, "", {}, hints, 0);
      expectedSnapshots.emplace(id, test.expected);
      const auto& notification = manager.all().back();
      TEST_CHECK(notification.id == id);
      const auto& snapshot = notification.imageData;
      ok = check(
               snapshot && snapshot->width == 1 && snapshot->height == 1 && snapshot->data == test.expected,
               std::string("wrong notification snapshot for ") + key + ": " + test.input
           )
          && ok;
      TEST_CHECK(manager.close(id, CloseReason::Expired));
    }
  }

  // History must keep the captured pixels even after the original images vanish.
  std::filesystem::remove(literal);
  std::filesystem::remove(space);
  const auto historyFile = temp.path / "history.json";
  TEST_CHECK(saveNotificationHistoryToFile(historyFile, manager.history(), 100, manager.changeSerial()));
  std::deque<NotificationHistoryEntry> restored;
  std::uint32_t nextId = 0;
  std::uint64_t serial = 0;
  TEST_CHECK(loadNotificationHistoryFromFile(historyFile, restored, nextId, serial));
  TEST_CHECK(restored.size() == expectedSnapshots.size());
  for (const auto& entry : restored) {
    const auto& snapshot = entry.notification.imageData;
    const auto& expected = expectedSnapshots.at(entry.notification.id);
    bool matches =
        snapshot && snapshot->width == 1 && snapshot->height == 1 && snapshot->data.size() == expected.size();
    if (matches) {
      // History uses lossy WebP; allow small color changes while distinguishing
      // the red and green source files unambiguously.
      for (std::size_t i = 0; i < expected.size(); ++i) {
        matches = std::abs(static_cast<int>(snapshot->data[i]) - static_cast<int>(expected[i])) <= 16 && matches;
      }
    }
    ok = check(matches, "wrong persisted snapshot for " + entry.notification.summary) && ok;
  }

  return ok ? 0 : 1;
}
