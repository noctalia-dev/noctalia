#pragma once

#include "config/config_types.h"

namespace noctalia::bar {

  [[nodiscard]] constexpr bool shouldPrewarmCompositorBlur(const BarConfig& config) noexcept {
    return config.compositorBlur;
  }

  [[nodiscard]] constexpr bool
  shouldPublishCompositorBlur(const BarConfig& config, bool contentVisuallyShown) noexcept {
    return config.compositorBlur && contentVisuallyShown;
  }

} // namespace noctalia::bar
