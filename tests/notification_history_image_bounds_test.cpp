#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "test_check.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  bool savesImageAsset(const std::filesystem::path& dir, NotificationImageData image) {
    std::deque<NotificationHistoryEntry> entries;
    entries.push_back({.notification = {.origin = NotificationOrigin::External, .imageData = std::move(image)}});
    const auto path = dir / "history.json";
    TEST_CHECK(saveNotificationHistoryToFile(path, entries, 2, 1));
    std::ifstream in(path);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return json.contains("image_file");
  }

} // namespace

int main() {
  const auto dir =
      std::filesystem::temp_directory_path() / ("noctalia-test-" + std::to_string(getpid()) + "-image-bounds");
  std::filesystem::create_directories(dir);

  // image-data comes from any D-Bus client, so its geometry must not be trusted over the buffer size.
  TEST_CHECK(!savesImageAsset(
      dir, {.width = 4096, .height = 4096, .rowStride = 4096 * 4, .data = std::vector<std::uint8_t>(16)}
  ));
  TEST_CHECK(!savesImageAsset(
      dir, {.width = 2, .height = 2, .rowStride = 4, .channels = 3, .data = std::vector<std::uint8_t>(64)}
  ));
  // Padded rows are valid as long as the last row is complete.
  TEST_CHECK(savesImageAsset(
      dir, {.width = 2, .height = 2, .rowStride = 12, .channels = 3, .data = std::vector<std::uint8_t>(12 + 6)}
  ));
  TEST_CHECK(!savesImageAsset(
      dir, {.width = 2, .height = 2, .rowStride = 12, .channels = 3, .data = std::vector<std::uint8_t>(12 + 5)}
  ));

  std::filesystem::remove_all(dir);
  return 0;
}
