// Locks schema::isKnownConfigPath — the path-resolution the settings GUI uses to
// keep its hand-written override paths (e.g. {"shell","ui_scale"}) tied to the
// single schema source. A schema rename that breaks one of these real paths fails
// here; bad paths must be rejected.

#include "config/schema/config_schema.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

using noctalia::config::schema::isKnownConfigPath;

int main() {
  // Real override paths the settings registry emits — leaf, nested sub-table,
  // enum, named-map, and bar/monitor forms.
  TEST_CHECK(isKnownConfigPath({"accessibility", "ui_scale"}));
  TEST_CHECK(isKnownConfigPath({"shell", "animation", "speed"}));
  TEST_CHECK(isKnownConfigPath({"shell", "panel_anchor_bar"}));
  TEST_CHECK(isKnownConfigPath({"shell", "panel", "control_center_placement"}));
  TEST_CHECK(isKnownConfigPath({"shell", "shadow", "alpha"}));
  TEST_CHECK(isKnownConfigPath({"shell", "screen_corners", "size"}));
  TEST_CHECK(isKnownConfigPath({"shell", "screenshot", "directory"}));
  TEST_CHECK(isKnownConfigPath({"system", "monitor", "cpu_poll_seconds"}));
  TEST_CHECK(isKnownConfigPath({"theme", "mode"}));
  TEST_CHECK(isKnownConfigPath({"theme", "templates", "enable_builtin_templates"}));
  TEST_CHECK(isKnownConfigPath({"wallpaper", "automation", "interval_seconds"}));
  TEST_CHECK(isKnownConfigPath({"wallpaper", "fill_color"}));
  TEST_CHECK(isKnownConfigPath({"dock", "icon_size"}));
  TEST_CHECK(isKnownConfigPath({"dock", "radius_top_left"}));
  TEST_CHECK(isKnownConfigPath({"dock", "monitor", "DP-1", "icon_size"}));
  TEST_CHECK(isKnownConfigPath({"dock", "monitor", "DP-1", "pinned"}));
  TEST_CHECK(isKnownConfigPath({"desktop_widgets", "enabled"}));
  TEST_CHECK(isKnownConfigPath({"desktop_widgets", "grid", "cell_size"}));
  TEST_CHECK(isKnownConfigPath({"desktop_widgets", "widget_order"}));
  TEST_CHECK(isKnownConfigPath({"desktop_widgets", "widget", "clock1", "type"}));
  TEST_CHECK(isKnownConfigPath({"desktop_widgets", "widget", "clock1", "settings", "format"}));
  TEST_CHECK(isKnownConfigPath({"osd", "scale"}));
  TEST_CHECK(isKnownConfigPath({"notification", "width"}));
  TEST_CHECK(isKnownConfigPath({"notification", "background_opacity"}));
  TEST_CHECK(isKnownConfigPath({"battery", "warning_threshold"}));
  TEST_CHECK(isKnownConfigPath({"calendar", "refresh_minutes"}));
  TEST_CHECK(isKnownConfigPath({"calendar", "account", "icloud", "provider"}));
  TEST_CHECK(isKnownConfigPath({"control_center", "calendar", "show_events_card"}));
  TEST_CHECK(isKnownConfigPath({"control_center", "media", "show_visualizer"}));
  TEST_CHECK(isKnownConfigPath({"nightlight", "temperature_day"}));
  TEST_CHECK(isKnownConfigPath({"location", "auto_locate"}));
  TEST_CHECK(isKnownConfigPath({"keybinds", "validate"}));
  TEST_CHECK(isKnownConfigPath({"control_center", "sidebar"}));
  TEST_CHECK(isKnownConfigPath({"hooks", "wallpaper_changed"}));
  // Bar: base field, container levels, and a resolved monitor-override field.
  TEST_CHECK(isKnownConfigPath({"bar", "default", "thickness"}));
  TEST_CHECK(isKnownConfigPath({"bar", "default", "concave_edge_corners"}));
  TEST_CHECK(isKnownConfigPath({"bar", "default", "position"}));
  TEST_CHECK(isKnownConfigPath({"bar", "default"}));
  TEST_CHECK(isKnownConfigPath({"bar", "default", "monitor", "DP-1", "thickness"}));
  TEST_CHECK(isKnownConfigPath({"bar", "default", "monitor", "DP-1", "concave_edge_corners"}));

  // Typos and bogus paths must NOT resolve.
  TEST_CHECK(!isKnownConfigPath({"shell", "ui_scl"}));                            // leaf typo
  TEST_CHECK(!isKnownConfigPath({"shell", "panel", "control_center_palcement"})); // nested typo
  TEST_CHECK(!isKnownConfigPath({"accessibilit", "ui_scale"}));                   // section typo
  TEST_CHECK(!isKnownConfigPath({"shell"}));                                      // bare section
  TEST_CHECK(!isKnownConfigPath({"dock", "radius_top_typo"}));
  TEST_CHECK(!isKnownConfigPath({"dock", "monitors"}));
  TEST_CHECK(!isKnownConfigPath({"dock", "monitor", "DP-1", "monitors"}));
  TEST_CHECK(!isKnownConfigPath({"desktop_widgets", "enabeld"}));
  TEST_CHECK(!isKnownConfigPath({"desktop_widgets", "grid", "cell_szie"}));
  TEST_CHECK(!isKnownConfigPath({"desktop_widgets", "widget", "clock1", "bogus"}));
  TEST_CHECK(!isKnownConfigPath({"bar", "default", "thicknesss"}));
  TEST_CHECK(!isKnownConfigPath({"bar", "default", "monitor", "DP-1", "bogus"}));
  TEST_CHECK(!isKnownConfigPath({}));

  return 0;
}
