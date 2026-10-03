#include "system/v4l2_camera.h"

#include "util/string_utils.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <set>
#include <string_view>
#include <system_error>

namespace {
  bool isNumber(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char ch) { return ch >= '0' && ch <= '9'; });
  }
} // namespace

bool v4l2CameraSameApp(std::string_view pipewireLabel, std::string_view comm) {
  const std::string left = StringUtils::toLower(StringUtils::pathTail(pipewireLabel));
  const std::string right = StringUtils::toLower(StringUtils::pathTail(comm));
  if (left.empty() || right.empty()) {
    return false;
  }
  if (left == right) {
    return true;
  }

  // TASK_COMM_LEN is 16, so /proc/<pid>/comm is at most 15 visible characters.
  constexpr std::size_t kCommMax = 15;
  if (left.size() == kCommMax && right.size() > kCommMax && right.starts_with(left)) {
    return true;
  }
  if (right.size() == kCommMax && left.size() > kCommMax && left.starts_with(right)) {
    return true;
  }
  return false;
}

int v4l2CameraPollTimeoutMs(
    std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point now
) noexcept {
  const auto remaining = std::clamp(
      std::chrono::ceil<std::chrono::milliseconds>(deadline - now), std::chrono::milliseconds::zero(),
      std::chrono::milliseconds{INT_MAX}
  );
  return static_cast<int>(remaining.count());
}

std::vector<std::string> v4l2CameraApps(const std::filesystem::path& videoClass, const std::filesystem::path& proc) {
  namespace fs = std::filesystem;
  std::set<fs::path> devices;
  std::error_code ec;
  for (fs::directory_iterator it(videoClass, ec), end; !ec && it != end; it.increment(ec)) {
    const auto device = it->path().filename().string();
    if (!device.starts_with("video") || !isNumber(std::string_view(device).substr(5))) {
      continue;
    }
    std::ifstream nameFile(it->path() / "name");
    std::string name;
    if (std::getline(nameFile, name) && !name.contains("Metadata")) {
      devices.insert(fs::path("/dev") / device);
    }
  }
  if (devices.empty()) {
    return {};
  }

  std::set<std::string> apps;
  for (fs::directory_iterator it(proc, ec), end; !ec && it != end; it.increment(ec)) {
    if (!isNumber(it->path().filename().string())) {
      continue;
    }
    std::error_code fdError;
    for (fs::directory_iterator fd(it->path() / "fd", fdError); !fdError && fd != fs::directory_iterator{};
         fd.increment(fdError)) {
      std::error_code linkError;
      const auto target = fs::read_symlink(fd->path(), linkError);
      if (linkError || !devices.contains(target)) {
        continue;
      }
      std::ifstream commFile(it->path() / "comm");
      std::string app;
      if (std::getline(commFile, app) && !app.empty()) {
        // PipeWire camera clients are identified through their links, not the broker's device FD.
        if (app != "pipewire" && app != "wireplumber") {
          apps.insert(std::move(app));
        }
      }
      break;
    }
  }
  return {apps.begin(), apps.end()};
}
