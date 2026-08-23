#pragma once

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::umbriel {

  class UmbrielEventHandler;

  // Owns the Umbriel IPC: stateless one-shot requests (fresh socket per call)
  // plus a single persistent event-stream socket whose messages are fanned out
  // to every registered UmbrielEventHandler. Backends are handlers and forward
  // their poll hooks here.
  class UmbrielRuntime {
  public:
    UmbrielRuntime() = default;
    ~UmbrielRuntime();

    UmbrielRuntime(const UmbrielRuntime&) = delete;
    UmbrielRuntime& operator=(const UmbrielRuntime&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] std::optional<nlohmann::json>
    requestCommand(std::string_view cmd, std::optional<std::string_view> arg = std::nullopt) const;
    [[nodiscard]] bool requestAction(std::string_view action) const;
    void refresh();
    void cleanup();

    // Event-stream dispatch. Handlers register on construction; the runtime owns
    // the socket and delivers each parsed event to every registered handler.
    void registerEventHandler(UmbrielEventHandler* handler);
    void unregisterEventHandler(UmbrielEventHandler* handler);
    [[nodiscard]] int pollFd() const noexcept { return m_eventSocketFd; }
    [[nodiscard]] short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
    [[nodiscard]] int pollTimeoutMs() const noexcept;
    void dispatchPoll(short revents);

  private:
    struct IpcReply;

    [[nodiscard]] IpcReply request(std::string_view payload) const;
    void ensureResolved() const;
    void resolveSocketPath() const;

    void connectIfNeeded();
    void closeSocket(bool scheduleReconnect);
    void scheduleReconnect();
    void readSocket();
    void parseMessages();
    [[nodiscard]] bool handleMessage(std::string_view line);
    void dispatchEvent(std::string_view event, const nlohmann::json& data) const;
    void notifyStreamReset() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
    std::vector<UmbrielEventHandler*> m_eventHandlers;
    int m_eventSocketFd = -1;
    std::vector<char> m_readBuffer;
    std::chrono::steady_clock::time_point m_nextReconnectAt;
    std::chrono::seconds m_reconnectBackoff{2};
  };

} // namespace compositors::umbriel
