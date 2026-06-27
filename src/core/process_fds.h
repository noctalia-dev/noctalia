#pragma once

#include <cstddef>
#include <string>

namespace ProcessFds {

  [[nodiscard]] std::string describeOpenFileDescriptors(std::size_t maxTargets = 8);

  // Raise the soft RLIMIT_NOFILE toward the hard limit. The default 1024 soft cap
  // is far too low for a long-running GPU client: the NVIDIA EGL/Wayland driver
  // accumulates internal sync_file fences over a session, and exhausting the soft
  // limit makes the Wayland connection fail fatally. Returns a human-readable
  // summary of the outcome for logging.
  [[nodiscard]] std::string raiseOpenFileLimit();

} // namespace ProcessFds
