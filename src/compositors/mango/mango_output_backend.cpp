#include "compositors/mango/mango_output_backend.h"

#include "core/process.h"
#include "wayland/wayland_connection.h"

#include <string>

namespace compositors::mango {

  bool setOutputPower(WaylandConnection& wayland, bool on) {
    bool launchedAny = false;
    for (const auto& output : wayland.outputs()) {
      if (output.connectorName.empty()) {
        continue;
      }
      if (process::runAsync(
              {"mmsg", "-s", "-d", std::string(on ? "enable_monitor," : "disable_monitor,") + output.connectorName}
          )) {
        launchedAny = true;
      }
    }
    return launchedAny;
  }

} // namespace compositors::mango
