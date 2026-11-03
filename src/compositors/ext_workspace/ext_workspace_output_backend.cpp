#include "compositors/ext_workspace/ext_workspace_output_backend.h"

#include "core/process/process.h"

#include <chrono>

namespace compositors::ext_workspace {

  bool setOutputPower(bool on) {
    return process::runSyncWithTimeout({"wlr-randr", on ? "--on" : "--off"}, std::chrono::milliseconds(500));
  }

} // namespace compositors::ext_workspace
