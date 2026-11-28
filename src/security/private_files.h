#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

// File I/O for the security key manager's private files (~/.config/Yubico/u2f_keys, ~/.ssh).
namespace security_hw::files {

  // Writes all of <data> to <fd>, retrying on EINTR.
  bool writeAll(int fd, std::string_view data);

  // Creates <path> with <mode>; never through or over an existing path.
  bool writeNewFile(const std::filesystem::path& path, std::string_view data, mode_t mode);

  // Replaces <path> whole or not at all: a private temporary file next to it, synced, then renamed
  // over it. A symbolic link at <path> is replaced, never written through.
  bool replaceFile(const std::filesystem::path& path, std::string_view text, mode_t mode);

  // The whole file, or nullopt when it can't be opened or is larger than <limit>. Never through a
  // symbolic link unless <follow>.
  std::optional<std::string> readLimited(const std::filesystem::path& path, bool follow, std::size_t limit);

} // namespace security_hw::files
