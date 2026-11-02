#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::umbriel {

  class UmbrielEventHandler;

  // The reply to one request line, read from a fresh connection.
  struct IpcReply {
    enum class Status : std::uint8_t {
      Unavailable,
      WriteFailed,
      ReadFailed,
      NoResponse,
      InvalidJson,
      Replied,
    };

    Status status = Status::Unavailable;
    std::optional<nlohmann::json> json;
  };

  // UMBRIEL_SOCKET, otherwise $XDG_RUNTIME_DIR/umbriel-$WAYLAND_DISPLAY.sock when that socket exists; empty when
  // neither applies.
  [[nodiscard]] std::string findSocketPath();
  // Sends `payload` (one newline-terminated request) and reads one reply line. Blocking and stateless, so it may run
  // on any thread.
  [[nodiscard]] IpcReply
  exchange(const std::string& socketPath, std::string_view payload, std::chrono::milliseconds timeout);

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
    [[nodiscard]] const std::string& socketPath() const;
    [[nodiscard]] std::optional<nlohmann::json> requestCommand(std::string_view cmd) const;
    // Runs an umbriel action. `fields` holds its typed arguments, such as {"window": id}.
    [[nodiscard]] bool requestAction(std::string_view action, nlohmann::json fields = nlohmann::json::object()) const;
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
    // Sends one JSON request; the reply when it is `ok`, otherwise nullopt.
    [[nodiscard]] std::optional<nlohmann::json> send(const nlohmann::json& request) const;
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
