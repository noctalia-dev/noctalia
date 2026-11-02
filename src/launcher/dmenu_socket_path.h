#pragma once

#include <string>

namespace noctalia::launcher {

  struct DmenuSocketPathResult {
    std::string path;
    std::string error;
  };

  DmenuSocketPathResult resolveDmenuSocketPath();

} // namespace noctalia::launcher
