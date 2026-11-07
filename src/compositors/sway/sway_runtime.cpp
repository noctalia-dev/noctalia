#include "compositors/sway/sway_runtime.h"

#include "system/process.h"

#include <cstdlib>

namespace compositors::sway {

  const std::string& SwayRuntime::socketPath() const {
    ensureResolved();
    return m_socketPath;
  }

  const std::string& SwayRuntime::msgCommand() const {
    ensureResolved();
    return m_msgCommand;
  }

  const std::string& SwayRuntime::outputCommand() const {
    ensureResolved();
    return m_outputCommand;
  }

  bool SwayRuntime::hasMsgCommand() const {
    ensureResolved();
    return !m_msgCommand.empty();
  }

  bool SwayRuntime::hasOutputCommand() const {
    ensureResolved();
    return !m_outputCommand.empty();
  }

  void SwayRuntime::refresh() {
    m_socketPath.clear();
    m_msgCommand.clear();
    m_outputCommand.clear();
    m_resolved = true;
    resolveSocketPath();
    resolveCommands();
  }

  void SwayRuntime::ensureResolved() const {
    if (!m_resolved) {
      m_resolved = true;
      resolveSocketPath();
      resolveCommands();
    }
  }

  void SwayRuntime::resolveSocketPath() const {
    if (const char* swaySock = std::getenv("SWAYSOCK"); swaySock != nullptr && swaySock[0] != '\0') {
      m_socketPath = swaySock;
      return;
    }
    if (const char* i3Sock = std::getenv("I3SOCK"); i3Sock != nullptr && i3Sock[0] != '\0') {
      m_socketPath = i3Sock;
    }
  }

  void SwayRuntime::resolveCommands() const {
    if (process::commandExists("swaymsg")) {
      m_msgCommand = "swaymsg";
    } else if (process::commandExists("i3-msg")) {
      m_msgCommand = "i3-msg";
    }

    if (process::commandExists("scrollmsg")) {
      m_outputCommand = "scrollmsg";
    } else {
      m_outputCommand = m_msgCommand;
    }
  }

} // namespace compositors::sway
