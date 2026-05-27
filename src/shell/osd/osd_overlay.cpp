#include "shell/osd/osd_overlay.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {

  constexpr Logger kLog("osd");

  constexpr int kHideDelayMs = (Style::animSlow * 3) + (Style::animFast * 2);

  struct SlideVector {
    float x = 0.0f;
    float y = 0.0f;
  };

  [[nodiscard]] float osdUiScale(const ConfigService* config) {
    if (config == nullptr) {
      return 1.0f;
    }
    const auto& shell = config->config().shell;
    const auto& osd = config->config().osd;
    return std::max(0.1f, shell.uiScale * osd.scale);
  }

  [[nodiscard]] float osdBackgroundOpacity(const ConfigService* config) {
    if (config == nullptr) {
      return 0.97f;
    }
    return std::clamp(config->config().osd.backgroundOpacity, 0.0f, 1.0f);
  }

  [[nodiscard]] bool isVerticalOrientation(const std::string& orientation) { return orientation == "vertical"; }

  // Base units at ui_scale=1; passive overlay (no hit targets), between bar and old OSD size.
  [[nodiscard]] float horizontalCardLength(float s) {
    return ((Style::controlHeight * 6) + Style::spaceMd + Style::spaceSm + Style::spaceXs) * s;
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
        return ((Style::controlHeight * 2) + Style::spaceLg + Style::spaceSm) * s;
      }
      return horizontalCardLength(s);
    }
    return (Style::controlHeight + Style::spaceSm) * s;
  }

  [[nodiscard]] std::uint32_t osdSurfaceWidth(float s, const std::string& orientation) {
    const float w = cardWidth(s, orientation) + (Style::spaceMd * s * 2.0f);
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(w))));
  }

  [[nodiscard]] std::int32_t horizontalLayerMarginFromScreenMargin(int offsetX, float scale) {
    return std::max(
        std::int32_t{0},
        static_cast<std::int32_t>(offsetX) - static_cast<std::int32_t>(std::lround(Style::spaceMd * scale))
    );
  }

  [[nodiscard]] std::uint32_t osdSurfaceHeight(float s, const std::string& orientation, bool showProgress) {
    const float h = cardHeight(s, orientation, showProgress) + (Style::spaceLg * s);
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(h))));
  }

  [[nodiscard]] float glyphSize(float s) { return (Style::fontSizeTitle + (Style::borderWidth * 4)) * s; }

  [[nodiscard]] float valueFontSize(float s) { return Style::fontSizeBody * s; }

  [[nodiscard]] float progressHeight(float s) { return (Style::spaceXs + (Style::borderWidth * 2)) * s; }

  [[nodiscard]] float verticalProgressWidth(float s) { return progressHeight(s) * 1.75f; }

  [[nodiscard]] float cardPadding(float s) { return Style::spaceMd * s; }

  [[nodiscard]] float innerGap(float s) { return (Style::spaceSm + (Style::spaceXs * 0.5f)) * s; }

  [[nodiscard]] float slideOffset(float s) { return Style::spaceSm * s; }

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

  SlideVector cardSlideVectorForPosition(const std::string& position, float offset) {
    if (isCenterPosition(position)) {
      return SlideVector{.x = isLeftPosition(position) ? -offset : offset, .y = 0.0f};
    }
    return SlideVector{.x = 0.0f, .y = (isBottomPosition(position) ? -offset : offset)};
  }

  std::string verticalValueText(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    bool previousWasSpace = false;
    for (const char c : text) {
      if (c == ' ' || c == '\t') {
        if (!previousWasSpace && !result.empty()) {
          result.push_back('\n');
        }
        previousWasSpace = true;
        continue;
      }
      result.push_back(c);
      previousWasSpace = false;
    }
    return result;
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

OsdOverlay::SurfaceMargins OsdOverlay::surfaceMarginsForPosition(const std::string& position) const {
  const int marginH = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetX) : 0;
  const int marginV = (m_config != nullptr) ? std::max(0, m_config->config().osd.offsetY) : 0;
  const float layoutScale = osdUiScale(m_config);
  const std::int32_t sideMargin = horizontalLayerMarginFromScreenMargin(marginH, layoutScale);

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

void OsdOverlay::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }

  const std::string position =
      (m_config != nullptr && !m_config->config().osd.position.empty()) ? m_config->config().osd.position : "top_right";
  const std::string orientation = (m_config != nullptr && !m_config->config().osd.orientation.empty())
      ? m_config->config().osd.orientation
      : "horizontal";
  const bool showProgress = m_content.showProgress;
  const float layoutScale = osdUiScale(m_config);

  if (!m_instances.empty()
      && (position != m_lastPosition || orientation != m_lastOrientation || showProgress != m_lastShowProgress)) {
    destroySurfaces();
  }

  if (!m_instances.empty() && std::abs(layoutScale - m_lastLayoutScale) > 1.0e-4f) {
    destroySurfaces();
  }

  if (!m_instances.empty() && std::abs(Style::cornerRadiusScale() - m_lastCornerRadiusScale) > 1.0e-4f) {
    destroySurfaces();
  }

  if (!m_instances.empty()) {
    for (auto& inst : m_instances) {
      if (inst->surface == nullptr) {
        continue;
      }
      const SurfaceMargins margins = surfaceMarginsForPosition(position);
      if (inst->surface->marginTop() != margins.top
          || inst->surface->marginRight() != margins.right
          || inst->surface->marginBottom() != margins.bottom
          || inst->surface->marginLeft() != margins.left) {
        inst->surface->setMargins(margins.top, margins.right, margins.bottom, margins.left);
      }
    }
  }

  if (!m_instances.empty() && m_instances.size() != m_wayland->outputs().size()) {
    destroySurfaces();
  }
  if (!m_instances.empty()) {
    return;
  }

  m_lastPosition = position;
  m_lastOrientation = orientation;
  m_lastShowProgress = showProgress;
  m_lastLayoutScale = layoutScale;
  m_lastCornerRadiusScale = Style::cornerRadiusScale();

  const auto surfaceWidth = osdSurfaceWidth(layoutScale, orientation);
  const auto surfaceHeight = osdSurfaceHeight(layoutScale, orientation, showProgress);

  for (const auto& output : m_wayland->outputs()) {
    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = output.scale;
    inst->uiLayoutScale = layoutScale;
    const SurfaceMargins margins = surfaceMarginsForPosition(position);

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
}

void OsdOverlay::buildScene(Instance& inst, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("OsdOverlay::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const float s = inst.uiLayoutScale;
  const bool vertical = isVerticalOrientation(m_lastOrientation);
  const float cw = cardWidth(s, m_lastOrientation);
  const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
  const float pad = cardPadding(s);
  const float gap = innerGap(s);
  const float border = Style::borderWidth * s;

  inst.sceneRoot = std::make_unique<Node>();
  inst.sceneRoot->setSize(w, h);
  inst.sceneRoot->setOpacity(0.0f);
  inst.surface->setSceneRoot(inst.sceneRoot.get());

  const float cardX = cardBaseX(w, cw);
  const float cardY = cardBaseYForPosition(m_lastPosition, h, ch);
  const float backgroundOpacity = osdBackgroundOpacity(m_config);

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
  inst.card = card.get();

  const auto rowProps = ui::FlexProps{
      .out = &inst.row,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = gap,
      .width = cw - (pad * 2.0f),
      .height = vertical ? ch - (pad * 2.0f) : ch,
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
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxWidth = vertical ? cw - (pad * 2.0f) : 0.0f,
      .fontWeight = FontWeight::Bold,
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
  inst.background->setFill(colorSpecFromRole(ColorRole::Surface, osdBackgroundOpacity(m_config)));

  const auto accentRole = m_content.overLimit ? ColorRole::Error : ColorRole::Primary;
  inst.glyph->setGlyph(m_content.icon);
  inst.glyph->setColor(colorSpecFromRole(accentRole));
  inst.progress->setVisible(m_content.showProgress);
  inst.progress->setFill(colorSpecFromRole(accentRole));
  inst.progress->setOrientation(vertical ? ProgressBarOrientation::Vertical : ProgressBarOrientation::Horizontal);
  inst.row->setJustify((vertical || !m_content.showProgress) ? FlexJustify::Center : FlexJustify::Start);
  inst.value->setFontSize(valueFontSize(s));
  inst.value->setColor(colorSpecFromRole(m_content.overLimit ? ColorRole::Error : ColorRole::OnSurface));
  inst.value->setTextAlign((vertical || !m_content.showProgress) ? TextAlign::Center : TextAlign::End);
  inst.value->setMaxWidth(vertical ? inst.card->width() - (cardPadding(s) * 2.0f) : 0.0f);
  inst.value->setMinWidth((!vertical && m_content.showProgress) ? inst.progressValueMinWidth : 0.0f);
  inst.value->setText((vertical && !m_content.showProgress) ? verticalValueText(m_content.value) : m_content.value);
  inst.progress->setRadius(osdProgressRadius(s));
  inst.progress->setProgress(m_content.progress);
  inst.row->layout(*m_renderContext);
  const float rowX = std::round((inst.card->width() - inst.row->width()) * 0.5f);
  const float rowY = std::round((inst.card->height() - inst.row->height()) * 0.5f);
  inst.row->setPosition(vertical ? rowX : cardPadding(s), rowY);
}

void OsdOverlay::animateInstance(Instance& inst) {
  if (inst.sceneRoot == nullptr) {
    return;
  }

  if (inst.hideAnimId != 0) {
    inst.animations.cancel(inst.hideAnimId);
    inst.hideAnimId = 0;
  }

  const float s = inst.uiLayoutScale;
  const float cw = cardWidth(s, m_lastOrientation);
  const float ch = cardHeight(s, m_lastOrientation, m_lastShowProgress);
  const float baseX = cardBaseX(inst.sceneRoot->width(), cw);
  const float baseY = cardBaseYForPosition(m_lastPosition, inst.sceneRoot->height(), ch);
  const SlideVector slide = cardSlideVectorForPosition(m_lastPosition, slideOffset(s));
  if (!inst.visible) {
    // During fast updates (e.g. slider drag), don't restart the show animation
    // every tick; keep the current show motion and only extend hide timing.
    if (inst.showAnimId == 0) {
      const float startOpacity = inst.sceneRoot->opacity();
      if (startOpacity == 0.0f) {
        inst.card->setPosition(baseX + slide.x, baseY + slide.y);
        if (inst.background != nullptr) {
          inst.background->setPosition(baseX + slide.x, baseY + slide.y);
        }
      }
      inst.showAnimId = inst.animations.animate(
          startOpacity, 1.0f, Style::animNormal, Easing::EaseOutCubic,
          [&inst, baseX, baseY, slide](float v) {
            inst.sceneRoot->setOpacity(v);
            inst.card->setPosition(baseX + (slide.x * (1.0f - v)), baseY + (slide.y * (1.0f - v)));
            if (inst.background != nullptr) {
              inst.background->setPosition(baseX + (slide.x * (1.0f - v)), baseY + (slide.y * (1.0f - v)));
            }
          },
          [&inst]() {
            inst.showAnimId = 0;
            inst.visible = true;
          }
      );
    }
  } else {
    inst.sceneRoot->setOpacity(1.0f);
    inst.card->setPosition(baseX, baseY);
    if (inst.background != nullptr) {
      inst.background->setPosition(baseX, baseY);
    }
  }

  inst.hideAnimId = inst.animations.animateTimer(
      1.0f, 0.0f, kHideDelayMs, Easing::Linear, [](float /*v*/) {},
      [this, &inst, baseX, baseY, slide]() {
        inst.hideAnimId = inst.animations.animate(
            1.0f, 0.0f, Style::animNormal, Easing::EaseInOutQuad,
            [&inst, baseX, baseY, slide](float v) {
              inst.sceneRoot->setOpacity(v);
              inst.card->setPosition(baseX + (slide.x * (1.0f - v)), baseY + (slide.y * (1.0f - v)));
              if (inst.background != nullptr) {
                inst.background->setPosition(baseX + (slide.x * (1.0f - v)), baseY + (slide.y * (1.0f - v)));
              }
            },
            [this, &inst]() {
              inst.hideAnimId = 0;
              inst.visible = false;
              DeferredCall::callLater([this]() {
                const bool allIdle = std::all_of(m_instances.begin(), m_instances.end(), [](const auto& i) {
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
