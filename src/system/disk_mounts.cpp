#include "system/disk_mounts.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace {

  // /proc/mounts escapes space, tab, newline and backslash as octal \NNN sequences.
  std::string unescapeMountField(std::string_view field) {
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
      if (field[i] == '\\'
          && i + 3 < field.size()
          && std::isdigit(static_cast<unsigned char>(field[i + 1])) != 0
          && std::isdigit(static_cast<unsigned char>(field[i + 2])) != 0
          && std::isdigit(static_cast<unsigned char>(field[i + 3])) != 0) {
        const int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0');
        out.push_back(static_cast<char>(value));
        i += 3;
      } else {
        out.push_back(field[i]);
      }
    }
    return out;
  }

} // namespace

std::vector<DiskMount> physicalDiskMounts(const std::filesystem::path& mountsFile) {
  std::ifstream file{mountsFile};
  if (!file.is_open()) {
    return {};
  }

  // Collapse btrfs subvolumes and bind mounts backed by the same device, preferring the shortest
  // mount path so the device root wins.
  std::unordered_map<std::string, DiskMount> bySource;
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream iss{line};
    std::string source;
    std::string path;
    std::string filesystem;
    if (!(iss >> source >> path >> filesystem)) {
      continue;
    }

    source = unescapeMountField(source);
    path = unescapeMountField(path);
    if (!source.starts_with("/dev/") || source.starts_with("/dev/loop") || filesystem == "squashfs") {
      continue;
    }
    if (path == "/boot" || path.starts_with("/boot/")) {
      continue;
    }

    const auto it = bySource.find(source);
    if (it == bySource.end() || path.size() < it->second.path.size()) {
      bySource[source] = DiskMount{.path = std::move(path), .source = source, .filesystem = std::move(filesystem)};
    }
  }

  std::vector<DiskMount> mounts;
  mounts.reserve(bySource.size());
  for (auto& entry : bySource) {
    mounts.push_back(std::move(entry.second));
  }
  std::ranges::sort(mounts, {}, &DiskMount::path);
  return mounts;
}
