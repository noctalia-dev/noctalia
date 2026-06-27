#include "compositors/mango/mango_output_backend.h"

#include "compositors/mango/mango_runtime.h"
#include "wayland/wayland_connection.h"

#include <string>

namespace compositors::mango {

  bool setOutputPower(MangoRuntime& runtime, WaylandConnection& wayland, bool on) {
    bool launchedAny = false;
    for (const auto& output : wayland.outputs()) {
      if (output.connectorName.empty()) {
        continue;
      }
      if (runtime.dispatch((std::string(on ? "enable_monitor," : "disable_monitor,") + output.connectorName))) {
        launchedAny = true;
      }
    }
    return launchedAny;
  }

} // namespace compositors::mango
