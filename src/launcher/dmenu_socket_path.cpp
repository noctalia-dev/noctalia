#include "launcher/dmenu_socket_path.h"

#include <cstdlib>

namespace noctalia::launcher {

  DmenuSocketPathResult resolveDmenuSocketPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] == '\0') {
      return {.path = {}, .error = "XDG_RUNTIME_DIR is not set"};
    }

    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || display[0] == '\0') {
      return {.path = {}, .error = "WAYLAND_DISPLAY is not set"};
    }

    return {.path = std::string(runtime) + "/noctalia-dmenu-" + display + ".sock", .error = {}};
  }

} // namespace noctalia::launcher
