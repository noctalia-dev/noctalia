#pragma once

// Wire format between the shell and noctalia-pam-helper.
//
// shell -> helper (stdin):  u8 version, u32 length, password bytes
// helper -> shell (stdout): u8 version, u8 Status, i32 PAM return code
//
// Both sides lead with kProtocolVersion so a running shell and a helper from
// a newer package fail cleanly instead of misreading each other. On a version
// mismatch the helper replies with its version only. The helper exits 0 on
// success, 1 on a reported failure and 2 on a protocol error.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace pam_helper {

  constexpr std::uint8_t kProtocolVersion = 1;
  constexpr std::size_t kMaxPasswordBytes = 64 * 1024;

  enum class Status : std::uint8_t {
    Success = 0,
    Failed = 1,
    UserUnavailable = 2,
    StartFailed = 3,
  };

  constexpr Status kLastStatus = Status::StartFailed;

  constexpr int kExitSuccess = 0;
  constexpr int kExitFailure = 1;
  constexpr int kExitProtocolError = 2;

  [[nodiscard]] inline bool writeAll(int fd, const void* data, std::size_t len) {
    auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = len;
    while (remaining > 0) {
      const ssize_t n = ::write(fd, bytes, remaining);
      if (n > 0) {
        bytes += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] inline bool readAll(int fd, void* data, std::size_t len) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t remaining = len;
    while (remaining > 0) {
      const ssize_t n = ::read(fd, bytes, remaining);
      if (n > 0) {
        bytes += static_cast<std::size_t>(n);
        remaining -= static_cast<std::size_t>(n);
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

} // namespace pam_helper
