#include "system/disk_mounts.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "disk_mounts_test: FAIL: {}", message);
      ++g_failures;
    }
  }

} // namespace

int main() {
  const auto fixture = std::filesystem::temp_directory_path() / ("noctalia-disk-mounts-" + std::to_string(::getpid()));
  {
    std::ofstream out{fixture};
    out
        << "/dev/nvme0n1p2 / btrfs rw 0 0\n"
        << "/dev/nvme0n1p2 /home btrfs rw 0 0\n"
        << "/dev/sdb1 /mnt/My\\040Disk ext4 rw 0 0\n"
        << "/dev/mapper/vg-data /srv/data xfs rw 0 0\n"
        << "/dev/sda1 /boot/efi vfat rw 0 0\n"
        << "/dev/loop0 /snap/app squashfs ro 0 0\n"
        << "proc /proc proc rw 0 0\n"
        << "malformed\n";
  }

  const auto mounts = physicalDiskMounts(fixture);
  std::filesystem::remove(fixture);

  expect(mounts.size() == 3, "only deduplicated physical data mounts should remain");
  if (mounts.size() == 3) {
    expect(
        mounts[0] == DiskMount{.path = "/", .source = "/dev/nvme0n1p2", .filesystem = "btrfs"},
        "the shortest mount for a source should win and root should sort first"
    );
    expect(
        mounts[1] == DiskMount{.path = "/mnt/My Disk", .source = "/dev/sdb1", .filesystem = "ext4"},
        "escaped mount paths should be decoded"
    );
    expect(
        mounts[2] == DiskMount{.path = "/srv/data", .source = "/dev/mapper/vg-data", .filesystem = "xfs"},
        "device source and filesystem should be preserved"
    );
  }
  expect(physicalDiskMounts(fixture).empty(), "an unreadable mounts file should return no mounts");

  return g_failures == 0 ? 0 : 1;
}
