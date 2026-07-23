#include "shell/screen_corners/screen_corners.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "ui/controls/screen_corner.h"
#include "wayland/wayland_connection.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kCornerAnchors[4] = {
      LayerShellAnchor::Top | LayerShellAnchor::Left,
      LayerShellAnchor::Top | LayerShellAnchor::Right,
      LayerShellAnchor::Bottom | LayerShellAnchor::Right,
      LayerShellAnchor::Bottom | LayerShellAnchor::Left,
  };

  ScreenCornerPosition cornerPosition(int cornerIndex) {
    switch (cornerIndex) {
    case 0:
      return ScreenCornerPosition::TopLeft;
    case 1:
      return ScreenCornerPosition::TopRight;
    case 2:
      return ScreenCornerPosition::BottomRight;
    case 3:
      return ScreenCornerPosition::BottomLeft;
    default:
      return ScreenCornerPosition::TopLeft;
    }
  }

  bool outputEligible(const WaylandOutput& output) noexcept {
    return output.done && output.output != nullptr && output.hasUsableGeometry();
  }

  std::size_t eligibleOutputCount(const WaylandConnection& wayland) {
    return static_cast<std::size_t>(std::ranges::count_if(wayland.outputs(), outputEligible));
  }

} // namespace

void ScreenCorners::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void ScreenCorners::onConfigReload() {
  if (m_config == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().shell.screenCorners;
  if (!cfg.enabled) {
    if (m_lastEnabled) {
      destroySurfaces();
      m_lastEnabled = false;
    }
    return;
  }

  // The bar and dock share the Top layer with the corners, and wlr-layer-shell stacks
  // same-layer surfaces by creation order with no way to restack afterwards. A config
  // change that makes either recreate its surfaces (a bar corner radius resizes the
  // surface, or a shadow change alters bar/dock surface metrics, for instance) puts
  // them above the corners, which then get clipped away wherever the bar sits.
  // Recreate ours too so they climb back on top; this callback is registered after
  // the bar's, so the rebuild lands last.
  const auto& changed = m_config->lastChange();
  const bool stackingMayHaveChanged = changed.bars || changed.widgets || changed.dock || changed.shell;

  if (cfg.enabled != m_lastEnabled
      || cfg.size != m_lastSize
      || m_instances.size() != eligibleOutputCount(*m_wayland)
      || stackingMayHaveChanged) {
    destroySurfaces();
    m_lastEnabled = cfg.enabled;
    m_lastSize = cfg.size;
    ensureSurfaces();
  }
}

void ScreenCorners::onOutputChange() {
  if (m_config == nullptr) {
    return;
  }
  const auto& cfg = m_config->config().shell.screenCorners;
  m_lastEnabled = cfg.enabled;
  m_lastSize = cfg.size;

  destroySurfaces();
  if (!cfg.enabled) {
    return;
  }
  ensureSurfaces();
}

void ScreenCorners::requestRedraw() {
  for (auto& inst : m_instances) {
    for (auto& corner : inst->corners) {
      if (corner.surface != nullptr) {
        corner.surface->requestRedraw();
      }
    }
  }
}

void ScreenCorners::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_config == nullptr) {
    return;
  }

  const auto& cfg = m_config->config().shell.screenCorners;
  if (!cfg.enabled || !m_instances.empty()) {
    return;
  }

  const auto size = static_cast<std::uint32_t>(std::clamp(cfg.size, 1, 100));

  for (const auto& output : m_wayland->outputs()) {
    if (!outputEligible(output)) {
      continue;
    }

    auto inst = std::make_unique<OutputInstance>();
    inst->output = output.output;

    bool ok = true;
    for (int i = 0; i < 4; ++i) {
      auto surfaceConfig = LayerSurfaceConfig{
          .nameSpace = "noctalia-screen-corner",
          .layer = LayerShellLayer::Top,
          .anchor = kCornerAnchors[i],
          .width = size,
          .height = size,
          .exclusiveZone = -1,
          .keyboard = LayerShellKeyboard::None,
          .defaultWidth = size,
          .defaultHeight = size,
      };

      auto& corner = inst->corners[i];
      corner.surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
      corner.surface->setRenderContext(m_renderContext);

      auto* cornerPtr = &corner;
      const int cornerIndex = i;

      corner.surface->setConfigureCallback([cornerPtr](std::uint32_t, std::uint32_t) {
        cornerPtr->surface->requestLayout();
      });
      corner.surface->setPrepareFrameCallback([this, cornerPtr, size, cornerIndex](bool, bool) {
        auto& target = cornerPtr->surface->renderTarget();
        const auto width = target.logicalWidth() == 0 ? size : target.logicalWidth();
        const auto height = target.logicalHeight() == 0 ? size : target.logicalHeight();
        if (cornerPtr->sceneRoot == nullptr || cornerPtr->builtWidth != width || cornerPtr->builtHeight != height) {
          UiPhaseScope layoutPhase(UiPhase::Layout);
          buildCornerScene(*cornerPtr, width, height, cornerIndex);
        }
      });

      if (!corner.surface->initialize(output.output)) {
        ok = false;
        break;
      }
      corner.surface->setInputRegion({});
    }

    if (ok) {
      m_instances.push_back(std::move(inst));
    }
  }
}

void ScreenCorners::destroySurfaces() { m_instances.clear(); }

void ScreenCorners::buildCornerScene(Corner& corner, std::uint32_t width, std::uint32_t height, int cornerIndex) {
  const float logicalWidth = static_cast<float>(std::max<std::uint32_t>(1, width));
  const float logicalHeight = static_cast<float>(std::max<std::uint32_t>(1, height));

  auto root = std::make_unique<ScreenCorner>();
  root->setSize(logicalWidth, logicalHeight);
  root->setColor(Color{0.0f, 0.0f, 0.0f, 1.0f});
  root->setCorner(cornerPosition(cornerIndex));
  root->setExponent(4.0f);
  root->setSoftness(1.5f);

  corner.sceneRoot = std::move(root);
  corner.builtWidth = width;
  corner.builtHeight = height;
  corner.surface->setSceneRoot(corner.sceneRoot.get());
}
