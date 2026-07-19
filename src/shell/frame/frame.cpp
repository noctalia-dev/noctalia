#include "shell/frame/frame.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "render/core/render_styles.h"
#include "shell/frame/frame_geometry.h"
#include "ui/controls/box.h"
#include "ui/controls/screen_corner.h"
#include "ui/palette.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

  // Superellipse exponent for the hole's corner wedges; matches ScreenCornerStyle's default.
  constexpr float kCornerExponent = 4.0f;

  // Reservation strips, in the order the anchors below are indexed.
  enum Edge : int { EdgeTop = 0, EdgeBottom = 1, EdgeLeft = 2, EdgeRight = 3 };

  constexpr std::uint32_t kEdgeAnchors[4] = {
      LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right,
      LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
      LayerShellAnchor::Left | LayerShellAnchor::Top | LayerShellAnchor::Bottom,
      LayerShellAnchor::Right | LayerShellAnchor::Top | LayerShellAnchor::Bottom,
  };

  [[nodiscard]] int edgeForPosition(std::string_view position) {
    if (position == "bottom") {
      return EdgeBottom;
    }
    if (position == "left") {
      return EdgeLeft;
    }
    if (position == "right") {
      return EdgeRight;
    }
    return EdgeTop;
  }

  bool outputEligible(const WaylandOutput& output) noexcept {
    return output.done && output.output != nullptr && output.hasUsableGeometry();
  }

  std::size_t eligibleOutputCount(const WaylandConnection& wayland) {
    return static_cast<std::size_t>(std::ranges::count_if(wayland.outputs(), outputEligible));
  }

  // The bar the frame defers to, for fill and for its edge alike. Deliberately independent of
  // `reserveSpace`: a bar occupies its edge whether or not it displaces windows, so the frame
  // must neither paint nor reserve there either way. Reservation only decides who owns the band
  // while the bar is hidden, which is what the bar-owns-band provider answers.
  [[nodiscard]] const BarConfig* styleBar(const Config& config) {
    for (const auto& bar : config.bars) {
      if (bar.enabled) {
        return &bar;
      }
    }
    return nullptr;
  }

} // namespace

void Frame::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

const ShellConfig::FrameConfig& Frame::frameConfig() const { return m_config->config().shell.frame; }

Frame::Insets Frame::holeInsets() const {
  return frame_geometry::holeInsets(
      frameConfig(), frame_geometry::styleBar(m_config->config()), barVisible(), barReserves()
  );
}

float Frame::fillOpacity() const {
  return frame_geometry::fillOpacity(frameConfig(), frame_geometry::styleBar(m_config->config()));
}

void Frame::onConfigReload() {
  if (m_config == nullptr) {
    return;
  }

  const auto& cfg = frameConfig();
  if (!cfg.enabled) {
    if (m_lastEnabled) {
      destroySurfaces();
      m_lastEnabled = false;
    }
    return;
  }

  // Same layer-shell stacking hazard as the screen corners: the bar and dock share the Top
  // layer and are stacked by creation order, so recreate whenever either may have been
  // rebuilt. The bar also feeds our insets, so a bar change is a geometry change for us
  // regardless.
  const auto& changed = m_config->lastChange();
  const bool stackingMayHaveChanged = changed.bars || changed.widgets || changed.dock;
  const auto insets = holeInsets();

  if (cfg != m_lastConfig
      || insets != m_lastInsets
      || m_instances.size() != eligibleOutputCount(*m_wayland)
      || stackingMayHaveChanged) {
    destroySurfaces();
    m_lastEnabled = cfg.enabled;
    m_lastConfig = cfg;
    m_lastInsets = insets;
    ensureSurfaces();
  }
}

void Frame::onOutputChange() {
  if (m_config == nullptr) {
    return;
  }
  const auto& cfg = frameConfig();
  m_lastEnabled = cfg.enabled;
  m_lastConfig = cfg;
  m_lastInsets = holeInsets();

  destroySurfaces();
  if (!cfg.enabled) {
    return;
  }
  ensureSurfaces();
}

void Frame::setBarStateProviders(std::function<bool()> visible, std::function<bool()> reserves) {
  m_barVisibleProvider = std::move(visible);
  m_barReservesProvider = std::move(reserves);
}

bool Frame::barVisible() const { return m_barVisibleProvider == nullptr || m_barVisibleProvider(); }

bool Frame::barReserves() const { return m_barReservesProvider == nullptr || m_barReservesProvider(); }

void Frame::onBarVisibilityChanged() {
  // Scene-only rebuild: the surfaces themselves are unchanged, and recreating them here would
  // reshuffle the layer-shell stacking on every hide and reveal.
  for (auto& inst : m_instances) {
    if (inst->visual == nullptr || inst->builtWidth == 0 || inst->builtHeight == 0) {
      continue;
    }
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildFrameScene(*inst, inst->builtWidth, inst->builtHeight);
    inst->visual->requestRedraw();
  }
}

void Frame::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->visual != nullptr) {
      inst->visual->requestRedraw();
    }
  }
}

void Frame::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_config == nullptr) {
    return;
  }

  const auto& cfg = frameConfig();
  if (!cfg.enabled || !m_instances.empty()) {
    return;
  }

  const Insets insets = holeInsets();
  const BarConfig* bar = styleBar(m_config->config());
  const int barEdge = bar != nullptr ? edgeForPosition(bar->position) : -1;
  const std::array<float, 4> edgeInsets{insets.top, insets.bottom, insets.left, insets.right};

  for (const auto& output : m_wayland->outputs()) {
    if (!outputEligible(output)) {
      continue;
    }

    auto inst = std::make_unique<OutputInstance>();
    inst->output = output.output;
    inst->fallbackWidth = static_cast<std::uint32_t>(std::max(0, output.effectiveLogicalWidth()));
    inst->fallbackHeight = static_cast<std::uint32_t>(std::max(0, output.effectiveLogicalHeight()));

    // Reservation strips first, so the visual surface is created last and stays on top of
    // them. They paint nothing; they exist only to carry an exclusive zone.
    bool ok = true;
    for (int edge = 0; edge < 4 && ok; ++edge) {
      if (edge == barEdge) {
        continue; // the bar already reserves this edge
      }
      const auto depth = static_cast<std::uint32_t>(std::max(0.0f, edgeInsets[static_cast<std::size_t>(edge)]));
      if (depth == 0) {
        continue;
      }

      const bool vertical = edge == EdgeLeft || edge == EdgeRight;
      auto reservationConfig = LayerSurfaceConfig{
          .nameSpace = "noctalia-frame-reserve",
          .layer = LayerShellLayer::Top,
          .anchor = kEdgeAnchors[edge],
          .width = vertical ? depth : 0,
          .height = vertical ? 0 : depth,
          .exclusiveZone = static_cast<std::int32_t>(depth),
          .keyboard = LayerShellKeyboard::None,
          .defaultWidth = vertical ? depth : 0,
          .defaultHeight = vertical ? 0 : depth,
      };

      auto& reservation = inst->reservations[edge];
      reservation = std::make_unique<LayerSurface>(*m_wayland, std::move(reservationConfig));
      reservation->setRenderContext(m_renderContext);
      if (!reservation->initialize(output.output)) {
        ok = false;
        break;
      }
      reservation->setInputRegion({});
    }

    if (!ok) {
      continue;
    }

    // The visual spans the whole output and ignores every exclusive zone, including the
    // strips above and the bar's, so the rails can paint the space they reserved.
    auto visualConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-frame",
        .layer = LayerShellLayer::Top,
        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right,
        .exclusiveZone = -1,
        .keyboard = LayerShellKeyboard::None,
    };

    inst->visual = std::make_unique<LayerSurface>(*m_wayland, std::move(visualConfig));
    inst->visual->setRenderContext(m_renderContext);

    auto* instPtr = inst.get();
    inst->visual->setConfigureCallback([instPtr](std::uint32_t, std::uint32_t) { instPtr->visual->requestLayout(); });
    inst->visual->setPrepareFrameCallback([this, instPtr](bool, bool) {
      auto& target = instPtr->visual->renderTarget();
      const auto width = target.logicalWidth() == 0 ? instPtr->fallbackWidth : target.logicalWidth();
      const auto height = target.logicalHeight() == 0 ? instPtr->fallbackHeight : target.logicalHeight();
      if (width == 0 || height == 0) {
        return;
      }
      if (instPtr->sceneRoot == nullptr || instPtr->builtWidth != width || instPtr->builtHeight != height) {
        UiPhaseScope layoutPhase(UiPhase::Layout);
        buildFrameScene(*instPtr, width, height);
      }
    });

    if (!inst->visual->initialize(output.output)) {
      continue;
    }
    // Fully click-through: the rails are decoration, and the hole must not swallow clicks
    // destined for the desktop underneath.
    inst->visual->setInputRegion({});

    m_instances.push_back(std::move(inst));
  }
}

void Frame::destroySurfaces() { m_instances.clear(); }

void Frame::buildFrameScene(OutputInstance& instance, std::uint32_t width, std::uint32_t height) {
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const Insets insets = holeInsets();

  const auto hole = frame_geometry::paintedHole(w, h, insets, frameConfig().bleed);
  const float holeX = hole.x;
  const float holeY = hole.y;
  const float holeRight = hole.x + hole.width;
  const float holeBottom = hole.y + hole.height;
  const float radius = frame_geometry::clampRadius(frameConfig().radius, hole);

  const Color fill = colorForRole(ColorRole::Surface, fillOpacity());
  const BarConfig* styling = frame_geometry::styleBar(m_config->config());
  const Color barFill =
      styling != nullptr ? colorForRole(ColorRole::Surface, std::clamp(styling->backgroundOpacity, 0.0f, 1.0f)) : fill;
  const auto barEdge = frame_geometry::cededEdge(styling, barVisible());

  auto root = std::make_unique<Node>();
  root->setSize(w, h);

  // The bar asks the compositor to blur behind it, so its translucent fill lands on a blurred
  // backdrop and stays flat across a patterned wallpaper. Without the same request our fill
  // tracks the wallpaper underneath and drifts from the bar's colour.
  std::vector<InputRect> blurRegion;
  const auto addBlur = [&](float x, float y, float rw, float rh) {
    blurRegion.push_back(
        InputRect{
            .x = static_cast<int>(std::lround(x)),
            .y = static_cast<int>(std::lround(y)),
            .width = static_cast<int>(std::lround(rw)),
            .height = static_cast<int>(std::lround(rh)),
        }
    );
  };

  const auto addRail = [&](const frame_geometry::Rect& r, const Color& color) {
    if (r.empty()) {
      return;
    }
    auto rail = std::make_unique<Box>();
    rail->setFill(color);
    rail->setSize(r.width, r.height);
    rail->setParticipatesInLayout(false);
    rail->setPosition(r.x, r.y);
    root->addChild(std::move(rail));
    addBlur(r.x, r.y, r.width, r.height);
  };

  // Geometry lives in frame_geometry.h so it is unit-testable without a compositor; this is
  // only the translation into scene nodes. Rails and caps are disjoint by construction, so
  // nothing double-blends at fractional opacity.
  for (const auto& rail : frame_geometry::rails(w, h, insets, hole, barEdge)) {
    addRail(rail, fill);
  }
  for (const auto& cap : frame_geometry::endCaps(w, h, insets, barEdge)) {
    addRail(cap, barFill);
  }

  if (radius > 0.0f) {
    // Each wedge fills its square except the rounded quarter facing into the hole, which is
    // what ScreenCorner's shader already draws.
    const auto addCorner = [&](float x, float y, ScreenCornerPosition position) {
      auto corner = std::make_unique<ScreenCorner>();
      corner->setColor(fill);
      corner->setCorner(position);
      corner->setExponent(kCornerExponent);
      corner->setSize(radius, radius);
      corner->setParticipatesInLayout(false);
      corner->setPosition(x, y);
      root->addChild(std::move(corner));

      // Strip-tessellate the wedge rather than blurring its bounding square, which would
      // smear a quarter-disc of desktop at each inner corner. Traces the same superellipse
      // the ScreenCorner shader fills.
      const bool fromTop = position == ScreenCornerPosition::TopLeft || position == ScreenCornerPosition::TopRight;
      const bool fromLeft = position == ScreenCornerPosition::TopLeft || position == ScreenCornerPosition::BottomLeft;
      const int rows = static_cast<int>(std::lround(radius));
      for (int i = 0; i < rows; ++i) {
        const float dy = radius - static_cast<float>(i);
        const float remainder = std::clamp(1.0f - std::pow(dy / radius, kCornerExponent), 0.0f, 1.0f);
        const float span = radius - (radius * std::pow(remainder, 1.0f / kCornerExponent));
        if (span < 1.0f) {
          continue;
        }
        const float rowY = fromTop ? y + static_cast<float>(i) : y + radius - 1.0f - static_cast<float>(i);
        addBlur(fromLeft ? x : x + radius - span, rowY, span, 1.0f);
      }
    };

    addCorner(holeX, holeY, ScreenCornerPosition::TopLeft);
    addCorner(holeRight - radius, holeY, ScreenCornerPosition::TopRight);
    addCorner(holeRight - radius, holeBottom - radius, ScreenCornerPosition::BottomRight);
    addCorner(holeX, holeBottom - radius, ScreenCornerPosition::BottomLeft);
  }

  // Only worth asking the compositor for a blur while something is actually translucent.
  // Only worth the compositor's work while something is actually translucent.
  if (fill.a < 0.999f || barFill.a < 0.999f) {
    instance.visual->setBlurRegion(blurRegion);
  } else {
    instance.visual->clearBlurRegion();
  }

  instance.sceneRoot = std::move(root);
  instance.builtWidth = width;
  instance.builtHeight = height;
  instance.visual->setSceneRoot(instance.sceneRoot.get());
}
