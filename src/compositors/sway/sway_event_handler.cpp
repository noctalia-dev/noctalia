#include "compositors/sway/sway_event_handler.h"

#include "compositors/sway/sway_runtime.h"

namespace compositors::sway {

  SwayEventHandler::SwayEventHandler(SwayRuntime& runtime) : m_runtime(runtime) {
    m_runtime.registerEventHandler(this);
  }

  SwayEventHandler::~SwayEventHandler() { m_runtime.unregisterEventHandler(this); }

} // namespace compositors::sway
