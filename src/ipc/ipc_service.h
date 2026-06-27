#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

class IpcService {
public:
  using Handler = std::function<std::string(const std::string& args)>;

  enum class HandlerVisibility {
    Public,
    Hidden,
  };

  IpcService() = default;
  ~IpcService();

  IpcService(const IpcService&) = delete;
  IpcService& operator=(const IpcService&) = delete;

  // Creates and binds the Unix socket. Returns false if it fails (IPC disabled).
  bool start();

  // Returns the listening fd, or -1 if not started.
  [[nodiscard]] int listenFd() const noexcept { return m_listenFd; }

  // Returns the socket path used.
  [[nodiscard]] const std::string& socketPath() const noexcept { return m_socketPath; }

  // Called by IpcPollSource when POLLIN fires on the listening fd.
  void dispatch();

  // Execute a command line using the same handler registry as socket IPC.
  [[nodiscard]] std::string execute(const std::string& line) const;

  // When set, relative paths in IPC handlers should resolve against this directory
  // (the caller's cwd) instead of the daemon's cwd. Only populated during execute().
  [[nodiscard]] const std::optional<std::string>& callerCwd() const noexcept { return m_callerCwd; }

  // Register a handler for a command name. The handler receives everything after
  // the first space as `args`. Must return a string ending with '\n'.
  // `usage` describes the command signature, e.g. "panel-toggle <id>".
  // `description` is a short human-readable explanation shown in --help.
  // Hidden handlers remain executable but are omitted from --help.
  void registerHandler(
      const std::string& command, Handler handler, std::string usage = {}, std::string description = {},
      HandlerVisibility visibility = HandlerVisibility::Public
  );

private:
  struct HandlerEntry {
    Handler fn;
    std::string usage;
    std::string description;
    HandlerVisibility visibility = HandlerVisibility::Public;
  };

  void handleConnection(int connFd);
  std::string buildHelp() const;
  [[nodiscard]] std::string executeParsed(const std::string& command, const std::string& args) const;
  [[nodiscard]] static std::string resolveSocketPath();

  int m_listenFd = -1;
  std::string m_socketPath;
  mutable std::optional<std::string> m_callerCwd;
  // Registration order is retained; --help output is sorted for display.
  std::vector<std::pair<std::string, HandlerEntry>> m_handlers;
};
