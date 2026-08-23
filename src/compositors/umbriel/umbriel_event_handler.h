#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace compositors::umbriel {

  class UmbrielRuntime;

  // Base for objects that consume Umbriel event-stream messages. Handlers
  // register themselves with the runtime on construction and unregister on
  // destruction. The socket owner (UmbrielWorkspaceBackend) reads the stream
  // and fans each parsed event out through the runtime to every registered
  // handler.
  class UmbrielEventHandler {
  public:
    explicit UmbrielEventHandler(UmbrielRuntime& runtime);
    virtual ~UmbrielEventHandler();

    UmbrielEventHandler(const UmbrielEventHandler&) = delete;
    UmbrielEventHandler& operator=(const UmbrielEventHandler&) = delete;

    virtual void handleEvent(std::string_view event, const nlohmann::json& data) = 0;
    // Called when the runtime tears the event stream down (e.g. cleanup): the
    // handler's cached state should be considered stale.
    virtual void handleStreamReset() {}

  protected:
    UmbrielRuntime& m_runtime;
  };

} // namespace compositors::umbriel
