#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "test_check.h"
#include "util/base64.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

  bool savesImageAsset(const std::filesystem::path& dir, const NotificationImageData& image) {
    std::deque<NotificationHistoryEntry> entries;
    entries.push_back({.notification = {.origin = NotificationOrigin::External, .imageData = image}});
    const auto path = dir / "history.json";
    TEST_CHECK(saveNotificationHistoryToFile(path, entries, 2, 1));
    std::ifstream in(path);
    const auto json = nlohmann::json::parse(in);
    const auto& savedImage = json.at("entries").at(0).at("notification").at("image_data");
    if (savedImage.contains("image_file")) {
      const auto asset = dir / "notification_history_assets" / savedImage.at("image_file").get<std::string>();
      TEST_CHECK(std::filesystem::is_regular_file(asset));
      TEST_CHECK(std::filesystem::file_size(asset) > 0);
      return true;
    }
    TEST_CHECK(savedImage.at("data_b64").get<std::string>() == Base64::encode(image.data));
    TEST_CHECK(savedImage.at("width") == image.width);
    TEST_CHECK(savedImage.at("height") == image.height);
    TEST_CHECK(savedImage.at("row_stride") == image.rowStride);
    TEST_CHECK(savedImage.at("channels") == image.channels);
    TEST_CHECK(savedImage.at("bits_per_sample") == image.bitsPerSample);
    TEST_CHECK(savedImage.at("has_alpha") == image.hasAlpha);
    return false;
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
  TEST_CHECK(savesImageAsset(dir, {.width = 2, .height = 2, .rowStride = 0, .data = std::vector<std::uint8_t>(16)}));

  std::filesystem::remove_all(dir);
  return 0;
}
