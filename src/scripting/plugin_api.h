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
  inline constexpr std::uint32_t kServiceLifecyclePluginApiVersion = 17;
  inline constexpr std::uint32_t kPanelFrameTickPluginApiVersion = 18;
  inline constexpr std::uint32_t kFormatTimeTimezonePluginApiVersion = 19;
  inline constexpr std::uint32_t kSoundPluginApiVersion = 20;
  inline constexpr std::uint32_t kPluginUiPropsPluginApiVersion = 21;
  inline constexpr std::uint32_t kModuleRequirePluginApiVersion = 22;
  inline constexpr std::uint32_t kAsyncFileReadPluginApiVersion = 23;
  inline constexpr std::uint32_t kDirectArgvPluginApiVersion = 24;
  inline constexpr std::uint32_t kWallpaperMaskPluginApiVersion = 25;
  inline constexpr std::uint32_t kGetSettingPluginApiVersion = 26;
  inline constexpr std::uint32_t kCurrentPluginApiVersion = kGetSettingPluginApiVersion;

  static_assert(kOldestSupportedPluginApiVersion <= kCurrentPluginApiVersion);

  [[nodiscard]] constexpr bool supportsPluginApiVersion(std::uint32_t version) {
    return version >= kOldestSupportedPluginApiVersion && version <= kCurrentPluginApiVersion;
  }

} // namespace scripting
