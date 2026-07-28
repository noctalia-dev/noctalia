#pragma once

#include <cstdint>

namespace scripting {

  inline constexpr std::uint32_t kOldestSupportedPluginApiVersion = 3;
  inline constexpr std::uint32_t kStringMapSettingPluginApiVersion = 6;
  inline constexpr std::uint32_t kAllowInsecureTlsPluginApiVersion = 7;
  inline constexpr std::uint32_t kPanelDismissOnOutsideClickPluginApiVersion = 8;
  inline constexpr std::uint32_t kUiCallbackClosurePluginApiVersion = 9;
  inline constexpr std::uint32_t kPanelKeyboardFocusPluginApiVersion = 10;
  inline constexpr std::uint32_t kPersistentPanelPluginApiVersion = 11;
  inline constexpr std::uint32_t kSystemStatsPluginApiVersion = 12;
  inline constexpr std::uint32_t kPanelCaptureKeysPluginApiVersion = 13;
  inline constexpr std::uint32_t kWidgetGestureActionsPluginApiVersion = 14;
  inline constexpr std::uint32_t kOpenSettingsPluginApiVersion = 15;
  inline constexpr std::uint32_t kExtendedSystemStatsPluginApiVersion = 16;
  inline constexpr std::uint32_t kCurrentPluginApiVersion = kExtendedSystemStatsPluginApiVersion;

  static_assert(kOldestSupportedPluginApiVersion <= kCurrentPluginApiVersion);

  [[nodiscard]] constexpr bool supportsPluginApiVersion(std::uint32_t version) {
    return version >= kOldestSupportedPluginApiVersion && version <= kCurrentPluginApiVersion;
  }

} // namespace scripting
