#include "compositors/compositor_detect.h"

#include "util/string_utils.h"

#include <cstdlib>
#include <string>

namespace compositors {

  namespace {

    [[nodiscard]] std::string buildEnvHint() {
      constexpr const char* vars[] = {"XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION"};
      std::string hint;
      for (const char* var : vars) {
        const char* value = std::getenv(var);
        if (value == nullptr || value[0] == '\0') {
          continue;
        }
        if (!hint.empty()) {
          hint += ':';
        }
        hint += value;
      }
      return hint;
    }

    [[nodiscard]] CompositorKind detectImpl() {
      // Compositor-set env vars are the most reliable signal.
      if (const char* v = std::getenv("LABWC_PID"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Labwc;
      }
      if (const char* v = std::getenv("TRIAD_SOCKET"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Triad;
      }
      if (const char* v = std::getenv("RIVER_WM"); v != nullptr && StringUtils::containsInsensitive(v, "triad")) {
        return CompositorKind::Triad;
      }
      if (const char* v = std::getenv("NIRI_SOCKET"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Niri;
      }
      if (const char* v = std::getenv("HYPRLAND_INSTANCE_SIGNATURE"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Hyprland;
      }
      if (const char* v = std::getenv("SWAYSOCK"); v != nullptr && v[0] != '\0') {
        return CompositorKind::Sway;
      }

      // Fall back to the desktop env hint (covers dwl-style compositors that don't expose a socket var).
      const std::string hint = buildEnvHint();
      if (StringUtils::containsInsensitive(hint, "triad")) {
        return CompositorKind::Triad;
      }
      if (StringUtils::containsInsensitive(hint, "niri")) {
        return CompositorKind::Niri;
      }
      if (StringUtils::containsInsensitive(hint, "hypr")) {
        return CompositorKind::Hyprland;
      }
      if (StringUtils::containsInsensitive(hint, "sway")) {
        return CompositorKind::Sway;
      }
      if (StringUtils::containsInsensitive(hint, "mango") || StringUtils::containsInsensitive(hint, "dwl")) {
        return CompositorKind::Mango;
      }
      if (StringUtils::containsInsensitive(hint, "labwc")) {
        return CompositorKind::Labwc;
      }
      return CompositorKind::Unknown;
    }

  } // namespace

  CompositorKind detect() {
    static const CompositorKind cached = detectImpl();
    return cached;
  }

  std::string_view name(CompositorKind kind) {
    switch (kind) {
    case CompositorKind::Triad:
      return "Triad";
    case CompositorKind::Niri:
      return "Niri";
    case CompositorKind::Hyprland:
      return "Hyprland";
    case CompositorKind::Sway:
      return "Sway";
    case CompositorKind::Mango:
      return "Mango";
    case CompositorKind::Labwc:
      return "Labwc";
    case CompositorKind::Unknown:
      return "Unknown";
    }
    return "Unknown";
  }

  std::string_view envHint() {
    static const std::string cached = buildEnvHint();
    return cached;
  }

} // namespace compositors
