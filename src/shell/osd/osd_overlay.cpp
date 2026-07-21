#include "shell/osd/osd_overlay.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/surface/edge_inset.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr Logger kLog("osd");

  constexpr int kHideDelayMs = Style::animSlow * 3 + Style::animFast * 2;

  enum class OsdRevealDir { FromLeft, FromRight, FromTop, FromBottom };

  [[nodiscard]] float osdContentOpacity(float reveal) {
    const float v = std::clamp(reveal, 0.0f, 1.0f);
    if (v <= 0.15f) {
      return 0.0f;
    }
    return std::clamp((v - 0.15f) / 0.85f, 0.0f, 1.0f);
  }

  [[nodiscard]] float osdUiScale(const ConfigService* config) {
    if (config == nullptr) {
      return 1.0f;
    }
    const auto& accessibility = config->config().accessibility;
    const auto& osd = config->config().osd;
    return std::max(0.1f, accessibility.uiScale * osd.scale);
  }

  [[nodiscard]] bool isOsdKindEnabled(const OsdKindsConfig& kinds, OsdKind kind) {
    switch (kind) {
    case OsdKind::Volume:
      return kinds.volume && kinds.volumeOutput;
    case OsdKind::Microphone:
      return kinds.volume && kinds.volumeInput;
    case OsdKind::Brightness:
      return kinds.brightness;
    case OsdKind::Wifi:
      return kinds.wifi;
    case OsdKind::Bluetooth:
      return kinds.bluetooth;
    case OsdKind::PowerProfile:
      return kinds.powerProfile;
    case OsdKind::Caffeine:
      return kinds.caffeine;
    case OsdKind::NightLight:
      return kinds.nightlight;
    case OsdKind::Dnd:
      return kinds.dnd;
    case OsdKind::LockKeys:
      return kinds.lockKeys;
    case OsdKind::KeyboardLayout:
      return kinds.keyboardLayout;
    case OsdKind::Media:
      return kinds.media;
    case OsdKind::Privacy:
      return kinds.privacy;
    case OsdKind::KeyboardBacklight:
      return kinds.keyboardBacklight;
    }
    return true;
  }

  [[nodiscard]] float osdBackgroundOpacity(const ConfigService* config) {
    if (config == nullptr) {
      return 0.97f;
    }
    return std::clamp(config->config().osd.backgroundOpacity, 0.0f, 1.0f);
  }

  [[nodiscard]] bool isVerticalOrientation(const std::string& orientation) { return orientation == "vertical"; }

  [[nodiscard]] std::string effectiveOsdOrientation(const OsdContent& content, const std::string& configOrientation) {
    if (!content.showProgress) {
      return "horizontal";
    }
    return configOrientation.empty() ? "horizontal" : configOrientation;
  }

  [[nodiscard]] std::string effectiveOsdPosition(
      const std::string& effectiveOrientation, const std::string& horizontalPosition,
      const std::string& verticalPosition
  ) {
    if (isVerticalOrientation(effectiveOrientation)) {
      return verticalPosition.empty() ? "top_center" : verticalPosition;
    }
    return horizontalPosition.empty() ? "top_center" : horizontalPosition;
  }

  // Base units at ui_scale=1; passive overlay (no hit targets), between bar and old OSD size.
  [[nodiscard]] float horizontalCardLength(float s) {
    return (Style::controlHeight * 6 + Style::spaceMd + Style::spaceSm + Style::spaceXs) * s;
  }

  [[nodiscard]] float cardWidth(float s, const std::string& orientation) {
    if (isVerticalOrientation(orientation)) {
      return (Style::controlHeight + Style::spaceLg + Style::spaceMd) * s;
    }
    return horizontalCardLength(s);
  }

  [[nodiscard]] float cardHeight(float s, const std::string& orientation, bool showProgress) {
    if (isVerticalOrientation(orientation)) {
      if (!showProgress) {
        return (Style::controlHeight * 2 + Style::spaceLg + Style::spaceSm) * s;
      }
      return horizontalCardLength(s);
    }
    return (Style::controlHeight + Style::spaceSm) * s;
  }

  [[nodiscard]] std::uint32_t osdSurfaceWidth(float s, const std::string& orientation, float innerPadX) {
    const float w = cardWidth(s, orientation) + innerPadX * 2.0f;
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(w))));
  }

  [[nodiscard]] std::uint32_t osdSurfaceHeight(float s, const std::string& orientation, bool showProgress) {
    const float h = cardHeight(s, orientation, showProgress) + Style::spaceLg * s;
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(h))));
  }

  [[nodiscard]] float glyphSize(float s) { return (Style::fontSizeTitle + Style::borderWidth * 4) * s; }

  [[nodiscard]] float valueFontSize(float s) { return Style::fontSizeBody * s; }

  [[nodiscard]] float progressHeight(float s) { return (Style::spaceXs + Style::borderWidth * 2) * s; }

  [[nodiscard]] float verticalProgressWidth(float s) { return progressHeight(s) * 1.75f; }

  [[nodiscard]] float cardPadding(float s) { return Style::spaceMd * s; }

  [[nodiscard]] float innerGap(float s) { return (Style::spaceSm + Style::spaceXs * 0.5f) * s; }

  [[nodiscard]] float osdCardRadius(float cw, float ch, float layoutScale) {
    const float maxR = std::min(cw, ch) * 0.5f;
    return std::min(maxR, Style::scaledRadiusXl(layoutScale));
  }

  [[nodiscard]] float osdProgressRadius(float layoutScale) {
    const float ph = progressHeight(layoutScale);
    return std::min(ph * 0.5f, Style::scaledRadiusSm(layoutScale));
  }

  [[nodiscard]] bool isBottomPosition(const std::string& position) { return position.starts_with("bottom_"); }

  [[nodiscard]] bool isCenterPosition(const std::string& position) { return position.starts_with("center_"); }

  [[nodiscard]] bool isLeftPosition(const std::string& position) { return position.ends_with("_left"); }

  [[nodiscard]] bool isRightPosition(const std::string& position) { return position.ends_with("_right"); }

  float cardBaseX(float surfaceWidth, float cardW) { return (surfaceWidth - cardW) * 0.5f; }

  float cardBaseYForPosition(const std::string& position, float surfaceHeight, float cardH) {
    if (isBottomPosition(position)) {
      return std::max(0.0f, surfaceHeight - cardH);
    }
    if (isCenterPosition(position)) {
      return (surfaceHeight - cardH) * 0.5f;
    }
    return 0.0f;
  }

  OsdRevealDir revealDirForPosition(const std::string& position) {
    if (isLeftPosition(position)) {
      return OsdRevealDir::FromLeft;
    }
    if (isRightPosition(position)) {
      return OsdRevealDir::FromRight;
    }
    if (isBottomPosition(position)) {
      return OsdRevealDir::FromBottom;
    }
    return OsdRevealDir::FromTop;
  }

} // namespace

void OsdOverlay::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void OsdOverlay::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void OsdOverlay::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void OsdOverlay::show(const OsdContent& content) {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  if (m_config != nullptr && !m_config->config().osd.enabled) {
    return;
  }
  if (m_config != nullptr && !isOsdKindEnabled(m_config->config().osd.kinds, content.kind)) {
    return;
  }

  m_content = content;
  ensureSurfaces();
  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    inst->showPending = true;
    inst->surface->requestUpdate();
  }
}

bool OsdOverlay::isVisible() const {
  return std::ranges::any_of(m_instances, [](const auto& inst) {
    return inst->visible || inst->showPending || inst->showAnimId != 0;
  });
}

OsdOverlay::SurfaceMargins OsdOverlay::surfaceMarginsForPosition(const std::string& position) const {
  const int marginH = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetX) : 0;
  const int marginV = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetY) : 0;
  const float layoutScale = osdUiScale(m_config);
  const std::int32_t sideMargin = shell::surface_edge_inset::resolve(marginH, Style::spaceMd * layoutScale).layerMargin;

  SurfaceMargins margins{
      .top = marginV,
      .right = sideMargin,
      .bottom = 0,
      .left = 0,
  };

  if (position == "top_left") {
    margins.right = 0;
    margins.left = sideMargin;
  } else if (position == "top_center") {
    margins.right = 0;
  } else if (position == "bottom_left") {
    margins.top = 0;
    margins.right = 0;
    margins.bottom = marginV;
    margins.left = sideMargin;
  } else if (position == "bottom_center") {
    margins.top = 0;
    margins.right = 0;
    margins.bottom = marginV;
  } else if (position == "bottom_right") {
    margins.top = 0;
    margins.bottom = marginV;
  } else if (position == "center_left") {
    margins.top = 0;
    margins.right = 0;
    margins.left = sideMargin;
  } else if (position == "center_right") {
    margins.top = 0;
    margins.bottom = 0;
  }

  return margins;
}

std::vector<std::string> OsdOverlay::osdMonitors() const {
  if (m_config == nullptr) {
    return {};
  }
  return m_config->config().osd.monitors;
}

bool OsdOverlay::shouldRenderOnOutput(const WaylandOutput& output) const {
  const auto selectedMonitors = osdMonitors();
  if (selectedMonitors.empty()) {
    return true;
  }
  return std::ranges::any_of(selectedMonitors, [&output](const std::string& match) {
    return outputMatchesSelector(match, output);
  });
}

void OsdOverlay::onOutputChange() {
  if (m_instances.empty()) {
    return;
  }
  ensureSurfaces();
  requestLayout();
}

void OsdOverlay::onConfigReload() { onOutputChange(); }

void OsdOverlay::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }

  const std::string configOrientation = (m_config != nullptr && !m_config->config().osd.orientation.empty())
      ? m_config->config().osd.orientation
      : "horizontal";
  const std::string horizontalPosition = (m_config != nullptr && !m_config->config().osd.position.empty())
      ? m_config->config().osd.position
      : "top_center";
  const std::string verticalPosition = (m_config != nullptr && !m_config->config().osd.positionVertical.empty())
      ? m_config->config().osd.positionVertical
      : "top_center";
  const bool showProgress = m_content.showProgress;
  const std::string orientation = effectiveOsdOrientation(m_content, configOrientation);
  const std::string position = effectiveOsdPosition(orientation, horizontalPosition, verticalPosition);
  const float layoutScale = osdUiScale(m_config);
  const auto selectedMonitors = osdMonitors();

  if (!m_instances.empty()
      && (position != m_lastPosition
          || orientation != m_lastOrientation
          || showProgress != m_lastShowProgress
          || selectedMonitors != m_lastMonitorSelectors)) {
    destroySurfaces();
  }

  if (!m_instances.empty() && std::abs(layoutScale - m_lastLayoutScale) > 1.0e-4f) {
    destroySurfaces();
  }

  if (!m_instances.empty() && std::abs(Style::cornerRadiusScale() - m_lastCornerRadiusScale) > 1.0e-4f) {
    destroySurfaces();
  }

  const int marginH = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetX) : 0;
  const float horizontalInnerPad =
      shell::surface_edge_inset::resolve(marginH, Style::spaceMd * layoutScale).innerPadding;
  const auto surfaceWidth = osdSurfaceWidth(layoutScale, orientation, horizontalInnerPad);
  const auto surfaceHeight = osdSurfaceHeight(layoutScale, orientation, showProgress);
  const SurfaceMargins margins = surfaceMarginsForPosition(position);

  m_lastPosition = position;
  m_lastOrientation = orientation;
  m_lastShowProgress = showProgress;
  m_lastLayoutScale = layoutScale;
  m_lastCornerRadiusScale = Style::cornerRadiusScale();
  m_lastMonitorSelectors = selectedMonitors;

  const bool anyConfiguredPresent =
      selectedMonitors.empty()
      || std::any_of(m_wayland->outputs().begin(), m_wayland->outputs().end(), [this](const WaylandOutput& output) {
           return output.done && output.output != nullptr && output.hasUsableGeometry() && shouldRenderOnOutput(output);
         });

  std::erase_if(m_instances, [this, anyConfiguredPresent](const std::unique_ptr<Instance>& inst) {
    if (inst->output == nullptr) {
      return true;
    }
    const WaylandOutput* wlOutput = m_wayland->findOutputByWl(inst->output);
    if (wlOutput == nullptr) {
      return true;
    }
    if (!wlOutput->done || !wlOutput->hasUsableGeometry()) {
      return true;
    }
    return anyConfiguredPresent && !shouldRenderOnOutput(*wlOutput);
  });

  for (auto& inst : m_instances) {
    if (inst->surface == nullptr) {
      continue;
    }
    if (inst->surface->marginTop() != margins.top
        || inst->surface->marginRight() != margins.right
        || inst->surface->marginBottom() != margins.bottom
        || inst->surface->marginLeft() != margins.left) {
      inst->surface->setMargins(margins.top, margins.right, margins.bottom, margins.left);
    }
    if (inst->surface->width() != surfaceWidth || inst->surface->height() != surfaceHeight) {
      inst->surface->requestSize(surfaceWidth, surfaceHeight);
    }
  }

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (anyConfiguredPresent && !shouldRenderOnOutput(output)) {
      continue;
    }

    auto existingIt = std::ranges::find_if(m_instances, [&output](const auto& inst) {
      return inst != nullptr && inst->output == output.output;
    });
    if (existingIt != m_instances.end()) {
      (*existingIt)->scale = output.scale;
      (*existingIt)->uiLayoutScale = layoutScale;
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = output.scale;
    inst->uiLayoutScale = layoutScale;

    std::uint32_t anchor = LayerShellAnchor::Top | LayerShellAnchor::Right;

    if (position == "top_left") {
      anchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    } else if (position == "top_center") {
      anchor = LayerShellAnchor::Top;
    } else if (position == "bottom_left") {
      anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    } else if (position == "bottom_center") {
      anchor = LayerShellAnchor::Bottom;
    } else if (position == "bottom_right") {
      anchor = LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    } else if (position == "center_left") {
      anchor = LayerShellAnchor::Left;
    } else if (position == "center_right") {
      anchor = LayerShellAnchor::Right;
    }

    auto surfaceConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-osd",
        .layer = LayerShellLayer::Overlay,
        .anchor = anchor,
        .width = surfaceWidth,
        .height = surfaceHeight,
        .exclusiveZone = 0,
        .marginTop = margins.top,
        .marginRight = margins.right,
        .marginBottom = margins.bottom,
        .marginLeft = margins.left,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeight,
        .prewarmBlur = true,
    };

    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
    inst->surface->setRenderContext(m_renderContext);
    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      instPtr->surface->requestLayout();
    });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
      prepareFrame(*instPtr, needsUpdate, needsLayout);
    });
    inst->surface->setFrameTickCallback([this, instPtr](float /*deltaMs*/) {
      if (instPtr->animations.hasActive()) {
        updateBlurRegion(*instPtr);
      }
    });
    inst->surface->setAnimationManager(&inst->animations);

    if (!inst->surface->initialize(output.output)) {
      kLog.warn("osd overlay: failed to initialize surface on {}", output.connectorName);
      continue;
    }

    inst->surface->setInputRegion({});
    inst->wlSurface = inst->surface->wlSurface();
    m_instances.push_back(std::move(inst));
  }
}

void OsdOverlay::destroySurfaces() {
  for (auto& inst : m_instances) {
    inst->animations.cancelAll();
  }
  m_instances.clear();
}

void OsdOverlay::prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(inst.sceneRoot->height())) != height;
  if (needsSceneBuild) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(inst, width, height);
  }

  if ((needsUpdate || needsLayout || needsSceneBuild) && inst.sceneRoot != nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    updateInstanceContent(inst);
  }

  if (inst.sceneRoot != nullptr && inst.background != nullptr) {
    const float corner = Style::cornerRadiusScale();
    if (std::abs(corner - inst.appliedCornerRadiusScale) > 1.0e-4f) {
      const float s = inst.uiLayoutScale;
      const float cw = cardWidth(s, m_lastOrientation);
      const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
      inst.background->setRadius(osdCardRadius(cw, ch, s));
      if (inst.progress != nullptr) {
        inst.progress->setRadius(osdProgressRadius(s));
      }
      inst.appliedCornerRadiusScale = corner;
      inst.surface->requestRedraw();
    }
  }

  if (needsUpdate && inst.showPending) {
    animateInstance(inst);
    inst.showPending = false;
  }

  // Keep blur publication after animation state/positions are applied for this frame.
  updateBlurRegion(inst);
}

void OsdOverlay::buildScene(Instance& inst, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("OsdOverlay::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float s = inst.uiLayoutScale;
  const bool vertical = isVerticalOrientation(m_lastOrientation);
  const float cw = cardWidth(s, m_lastOrientation);
  const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
  const float pad = cardPadding(s);
  const float gap = innerGap(s);

  inst.sceneRoot = std::make_unique<Node>();
  inst.sceneRoot->setSize(w, h);
  inst.sceneRoot->setOpacity(1.0f);
  inst.surface->setSceneRoot(inst.sceneRoot.get());

  const float cardX = cardBaseX(w, cw);
  const float cardY = cardBaseYForPosition(m_lastPosition, h, ch);
  const float backgroundOpacity = osdBackgroundOpacity(m_config);
  const bool drawBorder = m_config == nullptr || m_config->config().osd.border;
  const float border = drawBorder ? Style::borderWidth * s : 0.0f;

  inst.sceneRoot->addChild(
      ui::box({
          .out = &inst.background,
          .width = cw,
          .height = ch,
          .configure = [cardX, cardY, cw, ch, s, border, backgroundOpacity](Box& box) {
            box.setCardStyle();
            box.setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
            box.setBorder(colorSpecFromRole(ColorRole::Outline), border);
            box.setRadius(osdCardRadius(cw, ch, s));
            box.setPosition(cardX, cardY);
            box.setZIndex(0);
          },
      })
  );

  auto card = ui::box({
      .fill = clearColorSpec(),
      .width = cw,
      .height = ch,
      .configure = [cardX, cardY](Box& box) {
        box.setPosition(cardX, cardY);
        box.setZIndex(1);
      },
  });
  card->setClipChildren(true);
  inst.card = card.get();

  const auto rowProps = ui::FlexProps{
      .out = &inst.row,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = gap,
      .width = cw - pad * 2.0f,
      .height = vertical ? ch - pad * 2.0f : ch,
      .configure = [](Flex& flex) { flex.setZIndex(1); },
  };

  auto icon = ui::glyph({
      .out = &inst.glyph,
      .glyphSize = glyphSize(s),
      .color = colorSpecFromRole(ColorRole::Primary),
      .configure = [](Glyph& glyph) { glyph.setZIndex(1); },
  });

  auto value = ui::label({
      .out = &inst.value,
      .text = "100%",
      .fontSize = valueFontSize(s),
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxWidth = vertical ? cw - pad * 2.0f : 0.0f,
      .maxLines = 1,
      .textAlign = vertical ? TextAlign::Center : TextAlign::End,
      .configure = [](Label& label) { label.setZIndex(1); },
  });
  // Reserve enough width for "100%" so the progress bar doesn't shrink at max values.
  value->measure(*m_renderContext);
  inst.progressValueMinWidth = value->width();
  value->setMinWidth(vertical ? 0.0f : inst.progressValueMinWidth);

  const float ph = progressHeight(s);
  auto progress = ui::progressBar({
      .out = &inst.progress,
      .fill = colorSpecFromRole(ColorRole::Primary),
      .track = colorSpecFromRole(ColorRole::SurfaceVariant),
      .radius = osdProgressRadius(s),
      .orientation = vertical ? ProgressBarOrientation::Vertical : ProgressBarOrientation::Horizontal,
      .width = vertical ? verticalProgressWidth(s) : 0.0f,
      .height = vertical ? 0.0f : ph,
      .flexGrow = 1.0f,
      .configure = [](ProgressBar& progressBar) { progressBar.setZIndex(1); },
  });

  std::unique_ptr<Flex> row;
  if (vertical) {
    row = ui::column(rowProps, std::move(icon), std::move(progress), std::move(value));
  } else {
    row = ui::row(rowProps, std::move(icon), std::move(progress), std::move(value));
  }
  card->addChild(std::move(row));

  inst.sceneRoot->addChild(std::move(card));

  inst.appliedCornerRadiusScale = Style::cornerRadiusScale();
}

void OsdOverlay::updateInstanceContent(Instance& inst) {
  if (m_renderContext == nullptr
      || inst.card == nullptr
      || inst.row == nullptr
      || inst.background == nullptr
      || inst.glyph == nullptr
      || inst.value == nullptr
      || inst.progress == nullptr) {
    return;
  }

  const float s = inst.uiLayoutScale;
  const bool vertical = isVerticalOrientation(m_lastOrientation);
  // Card frame size is animated during reveal; measure layout against intrinsic size.
  const float cw = cardWidth(s, m_lastOrientation);
  const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
  inst.background->setFill(colorSpecFromRole(ColorRole::Surface, osdBackgroundOpacity(m_config)));

  const ColorRole accentRole = m_content.overLimit ? ColorRole::Error
      : m_content.inactive                         ? ColorRole::OnSurfaceVariant
                                                   : ColorRole::Primary;
  inst.glyph->setGlyph(m_content.icon);
  inst.glyph->setColor(colorSpecFromRole(accentRole));
  inst.progress->setVisible(m_content.showProgress);
  inst.progress->setFill(colorSpecFromRole(accentRole));
  inst.progress->setOrientation(vertical ? ProgressBarOrientation::Vertical : ProgressBarOrientation::Horizontal);
  inst.row->setJustify((vertical || !m_content.showProgress) ? FlexJustify::Center : FlexJustify::Start);
  inst.value->setFontSize(valueFontSize(s));
  const ColorRole valueRole = m_content.overLimit ? ColorRole::Error
      : m_content.inactive                        ? ColorRole::OnSurfaceVariant
                                                  : ColorRole::OnSurface;
  inst.value->setColor(colorSpecFromRole(valueRole));
  inst.value->setTextAlign((vertical || !m_content.showProgress) ? TextAlign::Center : TextAlign::End);
  // Text OSDs (media title, device name, ...) carry arbitrary-length values; cap them to the card
  // interior so they ellipsize within the padding instead of overflowing. Progress OSDs keep the
  // uncapped "100%" value so it can reserve minWidth beside the bar.
  const float horizontalValueMax = cw - cardPadding(s) * 2.0f - glyphSize(s) - innerGap(s);
  inst.value->setMaxWidth(
      vertical                      ? cw - cardPadding(s) * 2.0f
          : !m_content.showProgress ? std::max(0.0f, horizontalValueMax)
                                    : 0.0f
  );
  inst.value->setMinWidth((!vertical && m_content.showProgress) ? inst.progressValueMinWidth : 0.0f);
  inst.value->setText(m_content.value);
  inst.progress->setRadius(osdProgressRadius(s));
  inst.progress->setProgress(m_content.progress);
  inst.row->layout(*m_renderContext);
  const float rowX = std::round((cw - inst.row->width()) * 0.5f);
  const float rowY = std::round((ch - inst.row->height()) * 0.5f);
  inst.rowBaseX = vertical ? rowX : cardPadding(s);
  inst.rowBaseY = rowY;
  inst.row->setPosition(inst.rowBaseX, inst.rowBaseY);
}

void OsdOverlay::updateBlurRegion(Instance& inst) const {
  if (inst.surface == nullptr || inst.background == nullptr || inst.sceneRoot == nullptr) {
    return;
  }
  if (!inst.visible && !inst.showPending && inst.showAnimId == 0 && inst.hideAnimId == 0) {
    inst.surface->clearBlurRegion();
    return;
  }

  const int rx = static_cast<int>(std::floor(inst.background->x()));
  const int ry = static_cast<int>(std::floor(inst.background->y()));
  const int rw = std::max(1, static_cast<int>(std::ceil(inst.background->width())));
  const int rh = std::max(1, static_cast<int>(std::ceil(inst.background->height())));
  const float radius = osdCardRadius(inst.background->width(), inst.background->height(), inst.uiLayoutScale);
  inst.surface->setBlurRegion(Surface::tessellateRoundedRect(rx, ry, rw, rh, radius));
}

void OsdOverlay::applyReveal(Instance& inst, float reveal) {
  if (inst.card == nullptr || inst.background == nullptr || inst.sceneRoot == nullptr) {
    return;
  }

  const float s = inst.uiLayoutScale;
  const float cw = cardWidth(s, m_lastOrientation);
  const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
  const float baseX = cardBaseX(inst.sceneRoot->width(), cw);
  const float baseY = cardBaseYForPosition(m_lastPosition, inst.sceneRoot->height(), ch);
  const float r = std::clamp(reveal, 0.0f, 1.0f);

  if (inst.row != nullptr) {
    inst.row->setOpacity(osdContentOpacity(r));
  }

  // Grow the card from its anchored edge by clipping the visible extent; the rounded
  // background and content stay at the resting position so nothing slides past the
  // surface buffer (which the compositor would hard-clip into a flat edge).
  switch (revealDirForPosition(m_lastPosition)) {
  case OsdRevealDir::FromLeft: {
    const float vw = std::round(cw * r);
    inst.background->setPosition(baseX, baseY);
    inst.background->setFrameSize(vw, ch);
    inst.card->setPosition(baseX, baseY);
    inst.card->setFrameSize(vw, ch);
    inst.row->setPosition(inst.rowBaseX, inst.rowBaseY);
    break;
  }
  case OsdRevealDir::FromRight: {
    const float vw = std::round(cw * r);
    const float hw = cw - vw;
    inst.background->setPosition(baseX + hw, baseY);
    inst.background->setFrameSize(vw, ch);
    inst.card->setPosition(baseX + hw, baseY);
    inst.card->setFrameSize(vw, ch);
    inst.row->setPosition(inst.rowBaseX - hw, inst.rowBaseY);
    break;
  }
  case OsdRevealDir::FromTop: {
    const float vh = std::round(ch * r);
    inst.background->setPosition(baseX, baseY);
    inst.background->setFrameSize(cw, vh);
    inst.card->setPosition(baseX, baseY);
    inst.card->setFrameSize(cw, vh);
    inst.row->setPosition(inst.rowBaseX, inst.rowBaseY);
    break;
  }
  case OsdRevealDir::FromBottom: {
    const float vh = std::round(ch * r);
    const float hh = ch - vh;
    inst.background->setPosition(baseX, baseY + hh);
    inst.background->setFrameSize(cw, vh);
    inst.card->setPosition(baseX, baseY + hh);
    inst.card->setFrameSize(cw, vh);
    inst.row->setPosition(inst.rowBaseX, inst.rowBaseY - hh);
    break;
  }
  }
}

void OsdOverlay::animateInstance(Instance& inst) {
  if (inst.sceneRoot == nullptr) {
    return;
  }

  if (inst.hideAnimId != 0) {
    inst.animations.cancel(inst.hideAnimId);
    inst.hideAnimId = 0;
  }

  if (!inst.visible) {
    // During fast updates (e.g. slider drag), don't restart the show animation
    // every tick; keep the current show motion and only extend hide timing.
    if (inst.showAnimId == 0) {
      inst.sceneRoot->setOpacity(1.0f);
      applyReveal(inst, 0.0f);
      inst.showAnimId = inst.animations.animate(
          0.0f, 1.0f, Style::animNormal, Easing::EaseOutCubic, [this, &inst](float v) { applyReveal(inst, v); },
          [&inst]() {
            inst.showAnimId = 0;
            inst.visible = true;
          }
      );
    }
  } else {
    applyReveal(inst, 1.0f);
  }

  inst.hideAnimId = inst.animations.animateTimer(
      1.0f, 0.0f, kHideDelayMs, Easing::Linear, [](float /*v*/) {},
      [this, &inst]() {
        inst.hideAnimId = inst.animations.animate(
            1.0f, 0.0f, Style::animNormal, Easing::EaseInQuad, [this, &inst](float v) { applyReveal(inst, v); },
            [this, &inst]() {
              inst.hideAnimId = 0;
              inst.visible = false;
              DeferredCall::callLater([this]() {
                const bool allIdle = std::ranges::all_of(m_instances, [](const auto& i) {
                  return !i->visible && !i->showPending && i->showAnimId == 0 && i->hideAnimId == 0;
                });
                if (allIdle) {
                  destroySurfaces();
                }
              });
            }
        );
      }
  );
}
