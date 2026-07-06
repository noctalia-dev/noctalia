#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace compositors::niri {

  class NiriRuntime;

  // Base for objects that consume niri event-stream messages. Handlers register
  // themselves with the runtime on construction and unregister on destruction.
  // The socket owner (NiriWorkspaceBackend) reads the stream and fans each
  // parsed event out through the runtime to every registered handler.
  class NiriEventHandler {
  public:
    explicit NiriEventHandler(NiriRuntime& runtime);
    virtual ~NiriEventHandler();

    NiriEventHandler(const NiriEventHandler&) = delete;
    NiriEventHandler& operator=(const NiriEventHandler&) = delete;

    virtual void handleEvent(std::string_view key, const nlohmann::json& value) = 0;
    // Called when the runtime tears the event stream down (e.g. cleanup): the
    // handler's cached state should be considered stale.
    virtual void handleStreamReset() {}

  protected:
    NiriRuntime& m_runtime;
  };

} // namespace compositors::niri
