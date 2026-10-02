#include "system/v4l2_camera.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string_view>

namespace {
  bool isNumber(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char ch) { return ch >= '0' && ch <= '9'; });
  }
} // namespace

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
    for (fs::directory_iterator fd(it->path() / "fd", fdError); !fdError && fd != end; fd.increment(fdError)) {
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
