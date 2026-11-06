#include "pipewire/pipewire_error.h"

#include <cerrno>
#include <pipewire/core.h>

namespace noctalia::pipewire {

  ErrorDisposition classifyError(std::uint32_t objectId, int result) noexcept {
    if (objectId == PW_ID_CORE && result == -EPIPE) {
      return ErrorDisposition::Reconnect;
    }
    if (result == -ENOENT || result == -ESTALE) {
      return ErrorDisposition::StaleObject;
    }
    return ErrorDisposition::Report;
  }

} // namespace noctalia::pipewire
