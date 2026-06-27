#pragma once

#include <cstdint>
#include <string_view>

namespace compositors {

  enum class CompositorKind : std::uint8_t {
    Unknown = 0,
    Niri = 1,
    Hyprland = 2,
    Sway = 3,
    Mango = 4,
    Labwc = 5,
    Triad = 6,
    Dwl = 7,
    Kde = 8,
  };

  // Detected once per process from env vars. Cached after the first call.
  [[nodiscard]] CompositorKind detect();

  // Human-readable name (e.g. for diagnostics / hardware info). "Unknown" for unrecognized.
  [[nodiscard]] std::string_view name(CompositorKind kind);

  // Concatenated XDG_CURRENT_DESKTOP:XDG_SESSION_DESKTOP:DESKTOP_SESSION env hint for diagnostics.
  [[nodiscard]] std::string_view envHint();

  [[nodiscard]] inline bool isNiri() { return detect() == CompositorKind::Niri; }
  [[nodiscard]] inline bool isHyprland() { return detect() == CompositorKind::Hyprland; }
  [[nodiscard]] inline bool isSway() { return detect() == CompositorKind::Sway; }
  [[nodiscard]] inline bool isMango() { return detect() == CompositorKind::Mango; }
  [[nodiscard]] inline bool isLabwc() { return detect() == CompositorKind::Labwc; }
  [[nodiscard]] inline bool isTriad() { return detect() == CompositorKind::Triad; }
  [[nodiscard]] inline bool isDwl() { return detect() == CompositorKind::Dwl; }
  [[nodiscard]] inline bool isKde() { return detect() == CompositorKind::Kde; }

} // namespace compositors
