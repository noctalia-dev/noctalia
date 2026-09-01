#include "compositors/umbriel/umbriel_output_backend.h"

#include "compositors/umbriel/umbriel_runtime.h"

namespace compositors::umbriel {

  bool setOutputPower(UmbrielRuntime& runtime, bool on) { return runtime.requestAction(on ? "dpms-on" : "dpms-off"); }

} // namespace compositors::umbriel
