#pragma once

#include "cli/schema_msg.h"
#include "ipc/ipc_invocation_context.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class IpcActionEditorVisibility {
  Shown,
  Hidden,
};

struct IpcHandlerOptions {
  IpcActionEditorVisibility actionEditorVisibility = IpcActionEditorVisibility::Shown;
};

class IpcService {
public:
  using Handler = std::function<std::string(const std::string& args)>;
  using ActionEditorVisibility = IpcActionEditorVisibility;
  using HandlerOptions = IpcHandlerOptions;

  // A registered command, for callers that need to present or validate the command set.
  // Description views borrow static CLI schema storage.
  struct HandlerInfo {
    std::string_view command;
    // Argument spec without the verb, e.g. "<id> [context]". Empty when the command takes none.
    std::string_view args;
    std::string_view description;
    // Whether to offer this command in the action editor. Hidden commands remain available to IPC
    // and hand-written config.
    ActionEditorVisibility actionEditorVisibility = ActionEditorVisibility::Shown;
    // Whether the command moves one position along an ordered set. Bound to a scroll gesture,
    // one of those runs once per flick: the several notches an eager wheel movement emits are
    // one intent, not a request to skip that many entries.
    bool cycles = false;

    // "panel-toggle <id> [context]" — the form shown in --help.
    [[nodiscard]] std::string signature() const;
  };

  // Sets the invocation context for its lifetime, restoring the previous value on destruction so
  // that a handler which re-enters execute() nests correctly.
  class InvocationScope {
  public:
    InvocationScope(const IpcService& ipc, std::optional<IpcInvocationContext> context);
    ~InvocationScope();

    InvocationScope(const InvocationScope&) = delete;
    InvocationScope& operator=(const InvocationScope&) = delete;
    InvocationScope(InvocationScope&&) = delete;
    InvocationScope& operator=(InvocationScope&&) = delete;

  private:
    const IpcService& m_ipc;
    std::optional<IpcInvocationContext> m_previous;
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

  // Where the running command was invoked from, when it did not arrive over the socket.
  // Empty for socket-originated commands.
  [[nodiscard]] const std::optional<IpcInvocationContext>& invocationContext() const noexcept {
    return m_invocationContext;
  }

  // Registered commands, sorted by name.
  [[nodiscard]] std::vector<HandlerInfo> handlers() const;
  [[nodiscard]] bool hasHandler(std::string_view command) const noexcept;

  // Bind a handler to a canonical `msg` schema command. The handler receives everything after
  // the first space as `args` and must return a string ending with '\n'. Command identity and
  // help metadata come from the schema.
  void bind(const noctalia::cli::Command& command, Handler handler, HandlerOptions options = {});

  // A command that steps one position along an ordered set (workspaces, tracks, power profiles).
  // Bound like any other; on a scroll gesture it runs once per flick instead of once per notch,
  // so an eager wheel movement moves one position rather than several.
  void bindCycle(const noctalia::cli::Command& command, Handler handler, HandlerOptions options = {});

  // True when `command` was registered with bindCycle().
  [[nodiscard]] bool handlerCycles(std::string_view command) const noexcept;

private:
  struct HandlerEntry {
    Handler fn;
    std::string argsSpec;
    std::string_view description;
    ActionEditorVisibility actionEditorVisibility = ActionEditorVisibility::Shown;
    bool cycles = false;
  };

  void handleConnection(int connFd);
  std::string buildHelp() const;
  [[nodiscard]] std::string executeParsed(const std::string& command, const std::string& args) const;
  [[nodiscard]] static std::string resolveSocketPath();

  int m_listenFd = -1;
  std::string m_socketPath;
  mutable std::optional<std::string> m_callerCwd;
  mutable std::optional<IpcInvocationContext> m_invocationContext;
  // Registration order is retained; --help output is sorted for display.
  std::vector<std::pair<std::string_view, HandlerEntry>> m_handlers;
};
