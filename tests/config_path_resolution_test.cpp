// Locks schema::isKnownConfigPath — the path-resolution the settings GUI uses to
// keep its hand-written override paths (e.g. {"shell","ui_scale"}) tied to the
// single schema source. A schema rename that breaks one of these real paths fails
// here; bad paths must be rejected.

#include "config/schema/config_schema.h"

#include <cstdio>
#include <string>
#include <vector>

using noctalia::config::schema::isKnownConfigPath;

namespace {
  int g_failures = 0;

  std::string join(const std::vector<std::string>& path) {
    std::string out;
    for (const auto& seg : path) {
      out += (out.empty() ? "" : ".") + seg;
    }
    return out;
  }

  void expectKnown(const std::vector<std::string>& path) {
    if (!isKnownConfigPath(path)) {
      std::fprintf(stderr, "config_path_resolution: FAIL: expected known: %s\n", join(path).c_str());
      ++g_failures;
    }
  }

  void expectUnknown(const std::vector<std::string>& path) {
    if (isKnownConfigPath(path)) {
      std::fprintf(stderr, "config_path_resolution: FAIL: expected unknown: %s\n", join(path).c_str());
      ++g_failures;
    }
  }
} // namespace

int main() {
  // Real override paths the settings registry emits — leaf, nested sub-table,
  // enum, named-map, and bar/monitor forms.
  expectKnown({"accessibility", "ui_scale"});
  expectKnown({"shell", "animation", "speed"});
  expectKnown({"shell", "panel_anchor_bar"});
  expectKnown({"shell", "panel", "control_center_placement"});
  expectKnown({"shell", "shadow", "alpha"});
  expectKnown({"shell", "screen_corners", "size"});
  expectKnown({"shell", "screenshot", "directory"});
  expectKnown({"system", "monitor", "cpu_poll_seconds"});
  expectKnown({"theme", "mode"});
  expectKnown({"theme", "templates", "enable_builtin_templates"});
  expectKnown({"wallpaper", "automation", "interval_seconds"});
  expectKnown({"wallpaper", "fill_color"});
  expectKnown({"dock", "icon_size"});
  expectKnown({"dock", "radius_top_left"});
  expectKnown({"desktop_widgets", "enabled"});
  expectKnown({"desktop_widgets", "grid", "cell_size"});
  expectKnown({"desktop_widgets", "widget_order"});
  expectKnown({"desktop_widgets", "widget", "clock1", "type"});
  expectKnown({"desktop_widgets", "widget", "clock1", "settings", "format"});
  expectKnown({"osd", "scale"});
  expectKnown({"notification", "background_opacity"});
  expectKnown({"battery", "warning_threshold"});
  expectKnown({"calendar", "refresh_minutes"});
  expectKnown({"calendar", "account", "icloud", "provider"});
  expectKnown({"control_center", "calendar", "show_events_card"});
  expectKnown({"nightlight", "temperature_day"});
  expectKnown({"location", "auto_locate"});
  expectKnown({"keybinds", "validate"});
  expectKnown({"control_center", "sidebar"});
  expectKnown({"hooks", "wallpaper_changed"});
  // Bar: base field, container levels, and a resolved monitor-override field.
  expectKnown({"bar", "default", "thickness"});
  expectKnown({"bar", "default", "concave_edge_corners"});
  expectKnown({"bar", "default", "position"});
  expectKnown({"bar", "default"});
  expectKnown({"bar", "default", "monitor", "DP-1", "thickness"});
  expectKnown({"bar", "default", "monitor", "DP-1", "concave_edge_corners"});

  // Typos and bogus paths must NOT resolve.
  expectUnknown({"shell", "ui_scl"});                            // leaf typo
  expectUnknown({"shell", "panel", "control_center_palcement"}); // nested typo
  expectUnknown({"accessibilit", "ui_scale"});                   // section typo
  expectUnknown({"shell"});                                      // bare section
  expectUnknown({"dock", "radius_top_typo"});
  expectUnknown({"desktop_widgets", "enabeld"});
  expectUnknown({"desktop_widgets", "grid", "cell_szie"});
  expectUnknown({"desktop_widgets", "widget", "clock1", "bogus"});
  expectUnknown({"bar", "default", "thicknesss"});
  expectUnknown({"bar", "default", "monitor", "DP-1", "bogus"});
  expectUnknown({});

  if (g_failures == 0) {
    std::puts("config_path_resolution: all checks passed");
    return 0;
  }
  std::fprintf(stderr, "config_path_resolution: %d failure(s)\n", g_failures);
  return 1;
}
