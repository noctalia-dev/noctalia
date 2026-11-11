#pragma once

#include <cstdint>
#include <string_view>

namespace compositors::sway {

  class SwayRuntime;

  // Base for objects that consume the shared sway IPC connection. Handlers register with the
  // runtime on construction and unregister on destruction.
  class SwayEventHandler {
  public:
    explicit SwayEventHandler(SwayRuntime& runtime);
    virtual ~SwayEventHandler();

    SwayEventHandler(const SwayEventHandler&) = delete;
    SwayEventHandler& operator=(const SwayEventHandler&) = delete;

    // One i3-ipc frame: a reply to a request sent on the connection, or a subscribed event.
    virtual void handleMessage(std::uint32_t type, std::string_view payload) = 0;
    // Every frame of one read has been handled; lets a handler coalesce a burst of events.
    virtual void handleBatchEnd() {}
    // The connection was closed; state derived from it is stale.
    virtual void handleCleanup() {}
    // The connection dropped on its own (hang-up, read error, bad frame), after handleCleanup().
    virtual void handleDisconnect() {}

  protected:
    SwayRuntime& m_runtime;
  };

} // namespace compositors::sway
