#pragma once

namespace noctalia::pipewire {

  // Context property that keeps client.conf from loading libpipewire-module-rt (gated on
  // `module.rt = !false`). The shell does no realtime audio work, and that module rewrites the
  // process-wide RLIMIT_RTTIME from the realtime portal; a portal reporting 0 gets the shell SIGKILLed.
  inline constexpr const char* kContextRtModuleKey = "module.rt";
  inline constexpr const char* kContextRtModuleDisabled = "false";

} // namespace noctalia::pipewire
