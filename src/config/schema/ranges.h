#pragma once

#include "config/config_limits.h"
#include "config/schema/field.h"

// Numeric ranges for settings that have a UI slider. Each constant is the SINGLE
// source for both the parser's clamp (the schema `Field`'s `Range`) and the
// settings GUI slider bounds — so the two can never drift. `min`/`max`/`step` are
// the curated UI range; the parser ignores `step` (GUI metadata only).
namespace noctalia::config::schema {

  // Shared ranges for concepts that recur verbatim across many settings.
  inline constexpr Range<float> kUnitRange{0.0f, 1.0f, 0.01f};          // opacities, intensities, 0..1 factors
  inline constexpr Range<float> kScaleRange{0.5f, 2.5f, 0.05f};         // ui_scale, notification/osd scale
  inline constexpr Range<std::int64_t> kRefreshMinutesRange{5, 240, 5}; // calendar/weather refresh interval

  // Shell.
  inline constexpr Range<float> kAnimationSpeedRange{0.1f, 4.0f, 0.05f};
  inline constexpr Range<float> kCornerRadiusScaleRange{0.0f, 2.0f, 0.05f};
  inline constexpr Range<std::int64_t> kControlCenterWidthRange{600, 1200, 10};
  inline constexpr Range<std::int64_t> kScreenCornersSizeRange{1, 100, 1};
  inline constexpr Range<std::int64_t> kClipboardHistoryMaxEntriesRange{
      noctalia::config::kClipboardHistoryMinEntries,
      noctalia::config::kClipboardHistoryMaxEntries,
      noctalia::config::kClipboardHistoryStepEntries,
  };

  // Battery / wallpaper.
  inline constexpr Range<std::int64_t> kBatteryWarningThresholdRange{0, 100, 1};
  inline constexpr Range<float> kWallpaperTransitionDurationRange{100.0f, 30000.0f, 100.0f};
  inline constexpr Range<std::int64_t> kWallpaperAutomationIntervalRange{1, 86400, 1};

  // Dock.
  inline constexpr Range<std::int64_t> kDockIconSizeRange{16, 128, 1};
  inline constexpr Range<std::int64_t> kDockPaddingRange{0, 100, 1};
  inline constexpr Range<std::int64_t> kDockItemSpacingRange{0, 100, 1};
  inline constexpr Range<std::int64_t> kDockMarginEndsRange{0, 500, 1};
  inline constexpr Range<std::int64_t> kDockMarginEdgeRange{0, 100, 1};
  inline constexpr Range<std::int64_t> kDockRadiusRange{0, 80, 1}; // radius + each corner
  inline constexpr Range<float> kDockActiveScaleRange{0.1f, 1.75f, 0.05f};
  inline constexpr Range<float> kDockInactiveScaleRange{0.1f, 1.0f, 0.05f};
  inline constexpr Range<float> kDockMagnificationScaleRange{1.0f, 2.0f, 0.05f};

} // namespace noctalia::config::schema
