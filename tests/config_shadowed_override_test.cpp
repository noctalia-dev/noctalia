#include "config/config_merge.h"
#include "core/toml.h"

#include <algorithm>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "config_shadowed_override_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  [[nodiscard]] bool hasWarning(const noctalia::config::schema::Diagnostics& diag, std::string_view path) {
    return std::ranges::any_of(diag.entries, [path](const auto& entry) {
      return entry.severity == noctalia::config::schema::Diagnostics::Severity::Warning && entry.path == path;
    });
  }

  [[nodiscard]] std::size_t warningCount(const noctalia::config::schema::Diagnostics& diag) {
    return static_cast<std::size_t>(std::ranges::count_if(diag.entries, [](const auto& entry) {
      return entry.severity == noctalia::config::schema::Diagnostics::Severity::Warning;
    }));
  }

  [[nodiscard]] toml::table parse(std::string_view text) { return toml::parse(text); }

} // namespace

int main() {
  using noctalia::config::collectShadowedPlacementOverrides;

  {
    // Same value on both sides: the sidecar restates the hand-authored value, nothing is ignored.
    const auto base = parse(R"(
[desktop_widgets.widget.a]
cx = 100.0
cy = 200.0
type = "clock"
)");
    const auto overlay = parse(R"(
[desktop_widgets.widget.a]
cx = 100.0
cy = 200.0
type = "clock"
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(warningCount(diag) == 0, "identical values should not warn");
  }

  {
    // Differing placement values plus an overlay-only field: only the shadowed key warns.
    const auto base = parse(R"(
[desktop_widgets.widget.a]
cx = 100.0
cy = 200.0
type = "clock"
)");
    const auto overlay = parse(R"(
[desktop_widgets.widget.a]
cx = 640.0
cy = 360.0
output = "DP-1"
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(hasWarning(diag, "desktop_widgets.widget.a.cx"), "differing cx should warn");
    expect(hasWarning(diag, "desktop_widgets.widget.a.cy"), "differing cy should warn");
    expect(!hasWarning(diag, "desktop_widgets.widget.a.type"), "overlay missing type should not warn");
    expect(!hasWarning(diag, "desktop_widgets.widget.a.output"), "overlay-only key should not warn");
    expect(warningCount(diag) == 2, "exactly two warnings expected");
  }

  {
    // Overlay-only widget: pure state, no hand-authored config to shadow.
    const auto base = parse(R"(
[desktop_widgets]
widget_order = []
)");
    const auto overlay = parse(R"(
[desktop_widgets.widget.b]
cx = 1.0
cy = 2.0
type = "clock"
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(warningCount(diag) == 0, "overlay-only widget should not warn");
  }

  {
    // Lockscreen placement sections are covered too.
    const auto base = parse(R"(
[lockscreen_widgets.widget.c]
cx = 10.0
cy = 20.0
)");
    const auto overlay = parse(R"(
[lockscreen_widgets.widget.c]
cx = 30.0
cy = 20.0
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(hasWarning(diag, "lockscreen_widgets.widget.c.cx"), "lockscreen cx should warn");
    expect(!hasWarning(diag, "lockscreen_widgets.widget.c.cy"), "identical cy should not warn");
  }

  {
    // Other sections are deliberately out of scope: settings-UI overrides there are normal.
    const auto base = parse(R"(
[theme]
mode = "dark"
)");
    const auto overlay = parse(R"(
[theme]
mode = "light"
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(warningCount(diag) == 0, "non-placement sections should not warn");
  }

  {
    // A table replaced by a scalar (or vice versa) shadows the whole key once.
    const auto base = parse(R"(
[desktop_widgets.widget.d]
settings = { background = true }
)");
    const auto overlay = parse(R"(
[desktop_widgets.widget.d]
settings = false
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(hasWarning(diag, "desktop_widgets.widget.d.settings"), "kind change should warn once");
    expect(warningCount(diag) == 1, "kind change should produce a single warning");
  }

  {
    // Nested settings tables only warn for the leaves that actually differ.
    const auto base = parse(R"(
[desktop_widgets.widget.e.settings]
background = true
visualization_mode = "wave_rings"
)");
    const auto overlay = parse(R"(
[desktop_widgets.widget.e.settings]
background = true
visualization_mode = "bars"
)");
    noctalia::config::schema::Diagnostics diag;
    collectShadowedPlacementOverrides(base, overlay, diag);
    expect(hasWarning(diag, "desktop_widgets.widget.e.settings.visualization_mode"), "differing setting should warn");
    expect(!hasWarning(diag, "desktop_widgets.widget.e.settings.background"), "identical setting should not warn");
    expect(warningCount(diag) == 1, "exactly one nested warning expected");
  }

  if (g_failures != 0) {
    std::println(stderr, "config_shadowed_override_test: {} failure(s)", g_failures);
    return 1;
  }
  std::println("config_shadowed_override_test: all checks passed");
  return 0;
}
