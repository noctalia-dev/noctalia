#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct DiskMount {
  std::string path;
  std::string source;
  std::string filesystem;

  bool operator==(const DiskMount&) const = default;
};

// Physical block-device-backed filesystems, deduped by source and sorted by mount path.
// Pseudo filesystems, loop/squashfs mounts, and boot mounts are excluded.
[[nodiscard]] std::vector<DiskMount> physicalDiskMounts(const std::filesystem::path& mountsFile = "/proc/mounts");
