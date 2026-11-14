#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::sway {

  class SwayEventHandler;

  // i3-ipc message types used on the shared connection. Events carry the high bit.
  namespace ipc {
    inline constexpr std::uint32_t kRunCommand = 0;
    inline constexpr std::uint32_t kGetWorkspaces = 1;
    inline constexpr std::uint32_t kSubscribe = 2;
    inline constexpr std::uint32_t kGetTree = 4;
    inline constexpr std::uint32_t kWorkspaceEvent = 0x80000000U;
    inline constexpr std::uint32_t kWindowEvent = 0x80000003U;
    inline constexpr std::uint32_t kInputEvent = 0x80000015U;
  } // namespace ipc

  // Owns the sway IPC: socket path and msg command resolution plus a single persistent i3-ipc
  // connection subscribed to every event type a handler consumes. Replies to requests sent on the
  // connection arrive on the same stream; every frame goes to every registered SwayEventHandler.
  // SwayWorkspaceBackend connects the socket and forwards its poll hooks here.
  class SwayRuntime {
  public:
    SwayRuntime() = default;
    ~SwayRuntime();

    SwayRuntime(const SwayRuntime&) = delete;
    SwayRuntime& operator=(const SwayRuntime&) = delete;

    const std::string& socketPath() const;
    const std::string& msgCommand() const;
    const std::string& outputCommand() const;
    bool hasMsgCommand() const;
    bool hasOutputCommand() const;
    void refresh();

    [[nodiscard]] bool connectSocket();
    bool connected() const noexcept { return m_eventSocketFd >= 0; }
    void sendMessage(std::uint32_t type, std::string_view payload);
    void cleanup();

    void registerEventHandler(SwayEventHandler* handler);
    void unregisterEventHandler(SwayEventHandler* handler);
    int pollFd() const noexcept { return m_eventSocketFd; }
    void dispatchPoll(short revents);

  private:
    void ensureResolved() const;
    void resolveSocketPath() const;
    void resolveCommands() const;
    void disconnect();
    void readSocket();
    void parseMessages();

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
    mutable std::string m_msgCommand;
    mutable std::string m_outputCommand;
    int m_eventSocketFd = -1;
    std::vector<char> m_readBuffer;
    // The frame being dispatched, outside m_readBuffer so a handler whose request fails (and
    // clears the buffer through cleanup()) cannot invalidate the payload other handlers still read.
    std::string m_frame;
    std::vector<SwayEventHandler*> m_eventHandlers;
  };

} // namespace compositors::sway
