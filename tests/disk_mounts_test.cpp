#include "system/disk_mounts.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

  // Only deduplicated physical data mounts should remain.
  TEST_CHECK(mounts.size() == 3);
  // The shortest mount for a source should win and root should sort first.
  TEST_CHECK(mounts[0] == DiskMount{.path = "/", .source = "/dev/nvme0n1p2", .filesystem = "btrfs"});
  // Escaped mount paths should be decoded.
  TEST_CHECK(mounts[1] == DiskMount{.path = "/mnt/My Disk", .source = "/dev/sdb1", .filesystem = "ext4"});
  TEST_CHECK(mounts[2] == DiskMount{.path = "/srv/data", .source = "/dev/mapper/vg-data", .filesystem = "xfs"});
  // An unreadable mounts file should return no mounts.
  TEST_CHECK(physicalDiskMounts(fixture).empty());

  return 0;
}
