#pragma once

#include <cstdint>

namespace scripting {

  inline constexpr std::uint32_t kOldestSupportedPluginApiVersion = 3;
  inline constexpr std::uint32_t kStringMapSettingPluginApiVersion = 6;
  inline constexpr std::uint32_t kAllowInsecureTlsPluginApiVersion = 7;
  inline constexpr std::uint32_t kPanelDismissOnOutsideClickPluginApiVersion = 8;
  inline constexpr std::uint32_t kCurrentPluginApiVersion = kPanelDismissOnOutsideClickPluginApiVersion;

  static_assert(kOldestSupportedPluginApiVersion <= kCurrentPluginApiVersion);

  [[nodiscard]] constexpr bool supportsPluginApiVersion(std::uint32_t version) {
    return version >= kOldestSupportedPluginApiVersion && version <= kCurrentPluginApiVersion;
  }

} // namespace scripting
