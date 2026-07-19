// Geometry rules for the screen frame.
//
// Every case here corresponds to a defect that actually shipped during development, so the
// comments name the symptom rather than restating the assertion. The recurring mistake was
// conflating three separate questions about the bar — which edge it occupies, whether it is
// on screen, and whether it reserves space — so most of the file is about keeping those apart.

#include "shell/frame/frame_geometry.h"

#include <array>
#include <print>
#include <string>

using namespace frame_geometry;

namespace {

  int g_failures = 0;

  void fail(const std::string& message) {
    std::println(stderr, "frame_geometry: FAIL: {}", message);
    ++g_failures;
  }

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      fail(message);
    }
  }

  void expectRect(const Rect& got, const Rect& want, const std::string& what) {
    if (!(got == want)) {
      fail(what + ": got {" + std::to_string(got.x) + "," + std::to_string(got.y) + "," + std::to_string(got.width)
           + "," + std::to_string(got.height) + "} want {" + std::to_string(want.x) + "," + std::to_string(want.y) + ","
           + std::to_string(want.width) + "," + std::to_string(want.height) + "}");
    }
  }

  constexpr float kW = 3440.0f;
  constexpr float kH = 1440.0f;

  ShellConfig::FrameConfig frameCfg(int thickness = 8, int radius = 12, int bleed = 0) {
    ShellConfig::FrameConfig cfg;
    cfg.enabled = true;
    cfg.thickness = thickness;
    cfg.radius = radius;
    cfg.bleed = bleed;
    return cfg;
  }

  BarConfig barCfg(std::string position = "bottom", int thickness = 34) {
    BarConfig bar;
    bar.enabled = true;
    bar.position = std::move(position);
    bar.thickness = thickness;
    return bar;
  }

  void checkEdgeMapping() {
    expect(edgeForPosition("bottom") == Edge::Bottom, "edge: bottom");
    expect(edgeForPosition("left") == Edge::Left, "edge: left");
    expect(edgeForPosition("right") == Edge::Right, "edge: right");
    expect(edgeForPosition("top") == Edge::Top, "edge: top");
    // Unknown positions must fall through to "top", matching the bar's own default.
    expect(edgeForPosition("") == Edge::Top, "edge: empty falls back to top");
    expect(edgeForPosition("diagonal") == Edge::Top, "edge: unknown falls back to top");
  }

  void checkHoleInsets() {
    const auto cfg = frameCfg(8);
    const auto bar = barCfg("bottom", 34);

    // The bar IS the frame on its edge: that edge takes the bar's thickness, not the bar's
    // thickness plus the frame's. Getting this wrong double-insets the desktop.
    const auto shown = holeInsets(cfg, &bar, /*barVisible=*/true, /*barReserves=*/true);
    expect(shown.bottom == 34.0f, "insets: bar edge takes bar thickness, not bar + frame");
    expect(shown.left == 8.0f && shown.right == 8.0f && shown.top == 8.0f, "insets: other edges take frame thickness");

    // Hidden but still reserving (auto-hide keeps its exclusive zone): the band is still the
    // bar's, so the inset stays. Regression: the frame stopped covering an empty band.
    const auto hiddenReserving = holeInsets(cfg, &bar, false, true);
    expect(hiddenReserving.bottom == 34.0f, "insets: reserved band keeps the bar's thickness while hidden");

    // Hidden AND released (IPC hide, or a non-reserving bar): windows own the band, so the
    // frame falls back to its own thickness and still closes the border.
    const auto released = holeInsets(cfg, &bar, false, false);
    expect(released.bottom == 8.0f, "insets: released band falls back to frame thickness");

    // No bar at all — every edge is the frame's.
    const auto noBar = holeInsets(cfg, nullptr, false, false);
    expect(
        noBar.bottom == 8.0f && noBar.top == 8.0f && noBar.left == 8.0f && noBar.right == 8.0f,
        "insets: no bar means uniform frame thickness"
    );

    // Each bar position must inset its own edge and no other.
    const auto leftBar = barCfg("left", 30);
    const auto left = holeInsets(cfg, &leftBar, true, true);
    expect(left.left == 30.0f && left.right == 8.0f && left.top == 8.0f, "insets: left bar insets only the left edge");
    const auto topBar = barCfg("top", 30);
    const auto top = holeInsets(cfg, &topBar, true, true);
    expect(top.top == 30.0f && top.bottom == 8.0f, "insets: top bar insets only the top edge");
  }

  void checkFillOpacity() {
    auto cfg = frameCfg();
    auto bar = barCfg();
    bar.backgroundOpacity = 0.67f;

    cfg.matchBar = true;
    cfg.opacity = 1.0f;
    expect(fillOpacity(cfg, &bar) == 0.67f, "opacity: match_bar follows the bar and ignores `opacity`");

    // Regression: matching used to be routed through the reserving-bar lookup, so a bar with
    // reserve_space = false silently made the frame opaque instead of matching it. Matching
    // must not depend on reservation at all — hence no reservation argument here.
    cfg.matchBar = false;
    expect(fillOpacity(cfg, &bar) == 1.0f, "opacity: match_bar off uses the frame's own opacity");

    cfg.matchBar = true;
    expect(fillOpacity(cfg, nullptr) == 1.0f, "opacity: match_bar with no bar falls back to `opacity`");

    cfg.matchBar = false;
    cfg.opacity = 2.5f;
    expect(fillOpacity(cfg, &bar) == 1.0f, "opacity: clamped to 1");
    cfg.opacity = -1.0f;
    expect(fillOpacity(cfg, &bar) == 0.0f, "opacity: clamped to 0");
  }

  void checkCededEdge() {
    const auto bar = barCfg("bottom");
    // The frame cedes the edge only while the bar is actually on it. Painting there regardless
    // put the frame's rail on top of the bar's own pixels, because same-layer stacking follows
    // creation order and the frame is not reliably below the bar.
    expect(cededEdge(&bar, true) == Edge::Bottom, "ceded: bar on screen owns its edge");
    expect(cededEdge(&bar, false) == Edge::None, "ceded: bar off screen cedes nothing");
    expect(cededEdge(nullptr, true) == Edge::None, "ceded: no bar cedes nothing");
  }

  void checkPaintedHoleAndRadius() {
    const Insets reserved{8.0f, 8.0f, 8.0f, 34.0f};

    const auto noBleed = paintedHole(kW, kH, reserved, 0);
    expectRect(noBleed, Rect{8.0f, 8.0f, kW - 16.0f, kH - 42.0f}, "hole: no bleed");

    // Bleed pulls the hole inward on every edge so the frame covers the compositor's gap.
    const auto bled = paintedHole(kW, kH, reserved, 4);
    expectRect(bled, Rect{12.0f, 12.0f, kW - 24.0f, kH - 50.0f}, "hole: bleed pulls inward on all edges");

    // A radius past half the hole would make the corner arcs self-intersect.
    expect(clampRadius(20, bled) == 20.0f, "radius: kept when it fits");
    const Rect tiny{0.0f, 0.0f, 30.0f, 10.0f};
    expect(clampRadius(64, tiny) == 5.0f, "radius: clamped to half the shorter hole side");
    expect(clampRadius(-5, bled) == 0.0f, "radius: never negative");
  }

  void checkRailsCedeTheBarBand() {
    const Insets reserved{8.0f, 8.0f, 8.0f, 34.0f};
    const auto hole = paintedHole(kW, kH, reserved, 0);
    const auto barBand = kH - reserved.bottom;

    // With the bar on its edge, no rail may enter the band — a rail crossing it hid the bar's
    // widgets outright when the frame happened to stack above the bar.
    const auto ceded = rails(kW, kH, reserved, hole, Edge::Bottom);
    for (const auto& r : ceded) {
      expect(r.empty() || r.y + r.height <= barBand, "rails: nothing may enter the bar's band");
    }

    // With the bar gone, the bottom rail extends through the band so the border still closes.
    const auto reclaimed = rails(kW, kH, reserved, hole, Edge::None);
    expect(reclaimed[1].y + reclaimed[1].height == kH, "rails: bottom rail reaches the output edge once ceded");
  }

  void checkRailsAreDisjoint() {
    // Rails are painted at fractional opacity; any overlap double-blends into a visible seam.
    const std::array<Edge, 5> edges{Edge::None, Edge::Top, Edge::Bottom, Edge::Left, Edge::Right};
    for (const auto edge : edges) {
      for (const int bleed : {0, 4, 14}) {
        const Insets reserved{8.0f, 8.0f, 8.0f, 34.0f};
        const auto hole = paintedHole(kW, kH, reserved, bleed);
        const auto rs = rails(kW, kH, reserved, hole, edge);
        const auto caps = endCaps(kW, kH, reserved, edge);
        std::array<Rect, 6> all{rs[0], rs[1], rs[2], rs[3], caps[0], caps[1]};
        for (std::size_t i = 0; i < all.size(); ++i) {
          for (std::size_t j = i + 1; j < all.size(); ++j) {
            expect(!overlaps(all[i], all[j]), "rails/caps must stay disjoint at every edge and bleed");
          }
        }
      }
    }
  }

  void checkEndCapsUseReservedInset() {
    const Insets reserved{8.0f, 8.0f, 8.0f, 34.0f};

    // Caps are sized by the RESERVED inset. Sizing them by the painted (bled) inset reached
    // past the bar's edge and clipped its outermost widget.
    const auto caps = endCaps(kW, kH, reserved, Edge::Bottom);
    expectRect(caps[0], Rect{0.0f, kH - 34.0f, 8.0f, 34.0f}, "caps: bottom-left sized by reserved inset");
    expectRect(caps[1], Rect{kW - 8.0f, kH - 34.0f, 8.0f, 34.0f}, "caps: bottom-right sized by reserved inset");

    // A vertical bar is capped above and below instead.
    const Insets vertical{30.0f, 8.0f, 8.0f, 8.0f};
    const auto leftCaps = endCaps(kW, kH, vertical, Edge::Left);
    expectRect(leftCaps[0], Rect{0.0f, 0.0f, 30.0f, 8.0f}, "caps: left bar capped at the top");
    expectRect(leftCaps[1], Rect{0.0f, kH - 8.0f, 30.0f, 8.0f}, "caps: left bar capped at the bottom");

    // No bar means no caps to draw.
    const auto none = endCaps(kW, kH, reserved, Edge::None);
    expect(none[0].empty() && none[1].empty(), "caps: none without a bar edge");
  }

  void checkDegenerateOutputs() {
    // Tiny or zero-sized outputs must not produce negative or inverted rectangles.
    const auto cfg = frameCfg(64, 64, 64);
    const auto bar = barCfg("bottom", 34);
    for (const float w : {0.0f, 1.0f, 40.0f}) {
      for (const float h : {0.0f, 1.0f, 40.0f}) {
        const auto reserved = holeInsets(cfg, &bar, true, true);
        const auto hole = paintedHole(w, h, reserved, cfg.bleed);
        expect(hole.width >= 0.0f && hole.height >= 0.0f, "degenerate: hole never negative");
        expect(clampRadius(cfg.radius, hole) >= 0.0f, "degenerate: radius never negative");
        for (const auto& r : rails(w, h, reserved, hole, Edge::Bottom)) {
          expect(r.width >= 0.0f || r.empty(), "degenerate: rail width never negative");
        }
        for (const auto& c : endCaps(w, h, reserved, Edge::Bottom)) {
          expect(c.width >= 0.0f || c.empty(), "degenerate: cap width never negative");
        }
      }
    }
  }

  void checkStyleBarIgnoresReservation() {
    Config config;
    config.bars.clear();
    expect(styleBar(config) == nullptr, "styleBar: none when there are no bars");

    BarConfig disabled = barCfg();
    disabled.enabled = false;
    config.bars.push_back(disabled);
    expect(styleBar(config) == nullptr, "styleBar: skips disabled bars");

    // A bar that reserves nothing still sits on screen, so it still supplies the frame's edge
    // and fill. Requiring reserveSpace here made the frame reserve and paint the bar's own
    // edge, which stacked a rail under the bar and made it look taller.
    BarConfig noReserve = barCfg();
    noReserve.reserveSpace = false;
    config.bars.push_back(noReserve);
    const BarConfig* picked = styleBar(config);
    expect(picked != nullptr && !picked->reserveSpace, "styleBar: a non-reserving bar still counts");
  }

} // namespace

int main() {
  checkEdgeMapping();
  checkHoleInsets();
  checkFillOpacity();
  checkCededEdge();
  checkPaintedHoleAndRadius();
  checkRailsCedeTheBarBand();
  checkRailsAreDisjoint();
  checkEndCapsUseReservedInset();
  checkDegenerateOutputs();
  checkStyleBarIgnoresReservation();

  if (g_failures == 0) {
    std::println("frame_geometry: all checks passed");
    return 0;
  }
  std::println(stderr, "frame_geometry: {} failure(s)", g_failures);
  return 1;
}
