#include "compositors/mango/mango_output_backend.h"

#include "compositors/mango/mango_runtime.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <string>
#include <vector>

namespace compositors::mango {

  bool setOutputPower(MangoRuntime& runtime, WaylandConnection& wayland, bool on) {
    static std::vector<std::string> s_knownConnectors;
    for (const auto& output : wayland.outputs()) {
      if (!output.connectorName.empty()
          && std::ranges::find(s_knownConnectors, output.connectorName) == s_knownConnectors.end()) {
        s_knownConnectors.push_back(output.connectorName);
      }
    }

    bool allAccepted = !s_knownConnectors.empty();
    for (const auto& connector : s_knownConnectors) {
      const bool accepted = runtime.dispatch(std::string(on ? "wakeup_monitor," : "sleep_monitor,") + connector);
      allAccepted = accepted && allAccepted;
    }
    return allAccepted;
  }

} // namespace compositors::mango
