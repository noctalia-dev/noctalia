#include "system/v4l2_camera.h"

#include "util/string_utils.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <string_view>
#include <sys/inotify.h>
#include <system_error>
#include <unistd.h>

namespace {
  constexpr std::string_view kVideoClass = "/sys/class/video4linux";

  bool isNumber(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](char ch) { return ch >= '0' && ch <= '9'; });
  }

  bool isVideoNodeName(std::string_view name) { return name.starts_with("video") && isNumber(name.substr(5)); }

  // True for a video4linux class entry whose device name is readable and not a metadata node.
  bool isCaptureNode(const std::filesystem::path& classEntry) {
    std::ifstream nameFile(classEntry / "name");
    std::string name;
    return std::getline(nameFile, name) && !name.contains("Metadata");
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

std::vector<std::string> v4l2CameraApps(const std::filesystem::path& videoClass, const std::filesystem::path& proc) {
  namespace fs = std::filesystem;
  std::set<fs::path> devices;
  std::error_code ec;
  for (fs::directory_iterator it(videoClass, ec), end; !ec && it != end; it.increment(ec)) {
    const auto device = it->path().filename().string();
    if (isVideoNodeName(device) && isCaptureNode(it->path())) {
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

V4l2CameraWatch::V4l2CameraWatch() {
  // Only attribute changes: /dev sees constant opens of other nodes, which must not wake the shell.
  m_devWd = m_inotify.watch("/dev", IN_ATTRIB).value_or(-1);
  watchDevices();
}

bool V4l2CameraWatch::drain() {
  bool changed = false;
  bool overflow = false;
  m_inotify.drain([&](const inotify_event* event) {
    if ((event->mask & IN_Q_OVERFLOW) != 0) {
      overflow = true;
    } else if (event->wd != m_devWd) {
      changed = true;
    } else if (event->len > 0 && isVideoNodeName(event->name)) {
      // A node that just became readable may already be held open by an application.
      watchDevice(event->name);
      changed = true;
    }
  });
  if (overflow) {
    watchDevices();
    changed = true;
  }
  return changed;
}

void V4l2CameraWatch::watchDevices() {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(kVideoClass, ec), end; !ec && it != end; it.increment(ec)) {
    watchDevice(it->path().filename().string());
  }
}

void V4l2CameraWatch::watchDevice(std::string_view name) {
  if (!isVideoNodeName(name) || !isCaptureNode(std::filesystem::path(kVideoClass) / name)) {
    return;
  }
  // inotify needs read access; udev may not have applied the seat ACL yet, so retry on its IN_ATTRIB.
  const auto path = std::filesystem::path("/dev") / name;
  if (::access(path.c_str(), R_OK) == 0) {
    (void)m_inotify.watch(path, IN_OPEN | IN_CLOSE);
  }
}
