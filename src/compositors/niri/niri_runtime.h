#pragma once

#include <chrono>
#include <json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {

  class NiriEventHandler;

  // Owns the niri IPC: stateless request/response plus a single persistent
  // event-stream socket whose messages are fanned out to every registered
  // NiriEventHandler. Backends are handlers and forward their poll hooks here.
  class NiriRuntime {
  public:
    NiriRuntime() = default;
    ~NiriRuntime();

    NiriRuntime(const NiriRuntime&) = delete;
    NiriRuntime& operator=(const NiriRuntime&) = delete;

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::string& socketPath() const;
    [[nodiscard]] std::optional<nlohmann::json> requestJson(std::string_view request) const;
    [[nodiscard]] bool requestOk(std::string_view request, bool acceptNoResponse = false) const;
    [[nodiscard]] bool requestAction(const nlohmann::json& action, bool acceptNoResponse = false) const;
    void refresh();
    void cleanup();

    // Event-stream dispatch. Handlers register on construction; the runtime owns
    // the socket and delivers each parsed event to every registered handler.
    void registerEventHandler(NiriEventHandler* handler);
    void unregisterEventHandler(NiriEventHandler* handler);
    [[nodiscard]] int pollFd() const noexcept { return m_eventSocketFd; }
    [[nodiscard]] short pollEvents() const noexcept { return POLLIN | POLLHUP | POLLERR; }
    [[nodiscard]] int pollTimeoutMs() const noexcept;
    void dispatchPoll(short revents);

  private:
    struct IpcReply;

    [[nodiscard]] IpcReply request(std::string_view request) const;
    void ensureResolved() const;
    void resolveSocketPath() const;

    void connectIfNeeded();
    void closeSocket(bool scheduleReconnect);
    void scheduleReconnect();
    void readSocket();
    void parseMessages();
    [[nodiscard]] bool handleMessage(std::string_view line);
    void dispatchEvent(std::string_view key, const nlohmann::json& value) const;
    void notifyStreamReset() const;

    mutable bool m_resolved = false;
    mutable std::string m_socketPath;
    std::vector<NiriEventHandler*> m_eventHandlers;
    int m_eventSocketFd = -1;
    std::vector<char> m_readBuffer;
    std::chrono::steady_clock::time_point m_nextReconnectAt;
    std::chrono::seconds m_reconnectBackoff{2};
  };

} // namespace compositors::niri
