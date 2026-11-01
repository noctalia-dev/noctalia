#include "compositors/umbriel/umbriel_event_handler.h"

#include "compositors/umbriel/umbriel_runtime.h"

namespace compositors::umbriel {

  UmbrielEventHandler::UmbrielEventHandler(UmbrielRuntime& runtime) : m_runtime(runtime) {
    m_runtime.registerEventHandler(this);
  }

  UmbrielEventHandler::~UmbrielEventHandler() { m_runtime.unregisterEventHandler(this); }

} // namespace compositors::umbriel
