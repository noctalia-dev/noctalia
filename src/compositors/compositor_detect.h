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
    Dwl = 6,
    Kde = 7,
    Umbriel = 8,
  };

  // Detected once per process from env vars. Cached after the first call.
  CompositorKind detect();

  // Human-readable name (e.g. for diagnostics / hardware info). "Unknown" for unrecognized.
  std::string_view name(CompositorKind kind);

  // Concatenated XDG_CURRENT_DESKTOP:XDG_SESSION_DESKTOP:DESKTOP_SESSION env hint for diagnostics.
  std::string_view envHint();

  inline bool isNiri() { return detect() == CompositorKind::Niri; }
  inline bool isHyprland() { return detect() == CompositorKind::Hyprland; }
  inline bool isSway() { return detect() == CompositorKind::Sway; }
  inline bool isMango() { return detect() == CompositorKind::Mango; }
  inline bool isLabwc() { return detect() == CompositorKind::Labwc; }
  inline bool isDwl() { return detect() == CompositorKind::Dwl; }
  inline bool isKde() { return detect() == CompositorKind::Kde; }
  inline bool isUmbriel() { return detect() == CompositorKind::Umbriel; }

} // namespace compositors
