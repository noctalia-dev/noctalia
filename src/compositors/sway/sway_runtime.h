#pragma once

#include <string>

namespace compositors::sway {

  class SwayRuntime {
  public:
    SwayRuntime() = default;

    const std::string& socketPath() const;
    const std::string& msgCommand() const;
    const std::string& outputCommand() const;
    bool hasMsgCommand() const;
    bool hasOutputCommand() const;
    void refresh();

  private:
    void ensureResolved() const;
    void resolveSocketPath() const;
    void resolveCommands() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
    mutable std::string m_msgCommand;
    mutable std::string m_outputCommand;
  };

} // namespace compositors::sway
