#include "shell/tooltip/tooltip_manager.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

  constexpr Logger kLog("tooltip");

  constexpr auto kShowDelay = std::chrono::milliseconds(500);
  constexpr float kMaxContentWidth = 280.0f;
  constexpr int kMaxTextLines = 3;
  constexpr float kTableMinPeerColumnWidth = 80.0f;
  constexpr float kPadH = Style::spaceMd;
  constexpr float kPadV = Style::spaceSm;
  constexpr float kTableGap = Style::spaceXs;
  constexpr float kTableColumnGap = Style::spaceMd;
  constexpr float kBorder = Style::borderWidth;

  struct TableColumnWidths {
    float key = 0.0f;
    float value = 0.0f;
  };

  TableColumnWidths fitTableColumns(float naturalKeyW, float naturalValueW) {
    const float availableW = std::max(0.0f, kMaxContentWidth - kTableColumnGap);
    if (availableW <= 0.0f) {
      return {};
    }

    const float halfW = availableW * 0.5f;
    const float peerReserveW = std::min(kTableMinPeerColumnWidth, halfW);
    const float columnMaxW = std::max(0.0f, availableW - peerReserveW);

    TableColumnWidths widths{
        .key = std::min(naturalKeyW, halfW),
        .value = std::min(naturalValueW, halfW),
    };

    float remainingW = std::max(0.0f, availableW - widths.key - widths.value);
    if (remainingW <= 0.0f) {
      return widths;
    }

    float keyNeed = std::max(0.0f, std::min(naturalKeyW, columnMaxW) - widths.key);
    float valueNeed = std::max(0.0f, std::min(naturalValueW, columnMaxW) - widths.value);
    const float totalNeed = keyNeed + valueNeed;
    if (totalNeed <= 0.0f) {
      return widths;
    }

    const float keyDelta = std::min(keyNeed, remainingW * (keyNeed / totalNeed));
    widths.key += keyDelta;
    remainingW -= keyDelta;
    keyNeed -= keyDelta;

    const float valueDelta = std::min(valueNeed, remainingW);
    widths.value += valueDelta;
    remainingW -= valueDelta;

    if (remainingW > 0.0f && keyNeed > 0.0f) {
      widths.key += std::min(keyNeed, remainingW);
    }

    return widths;
  }

  PopupSurfaceConfig buildTooltipAnchorConfig(const InputArea* area) {
    const Node* anchorNode = area->tooltipAnchorNode();
    const Node* boundsNode = anchorNode != nullptr ? anchorNode : area;
    float absX = 0.0f;
    float absY = 0.0f;
    Node::absolutePosition(boundsNode, absX, absY);

    TooltipAnchorInsets inset{};
    if (area->hasTooltipAnchorInsets()) {
      inset = area->tooltipAnchorInsets();
    }
    const float iconX = absX + inset.left;
    const float iconY = absY + inset.top;
    const float iconW = std::max(1.0f, boundsNode->width() - inset.left - inset.right);
    const float iconH = std::max(1.0f, boundsNode->height() - inset.top - inset.bottom);

    const auto gap = static_cast<std::int32_t>(std::lround(Style::spaceSm));

    float anchorX = absX;
    float anchorY = absY;
    float anchorW = boundsNode->width();
    float anchorH = boundsNode->height();
    std::uint32_t anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
    std::uint32_t gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
    std::int32_t offsetX = 0;
    auto offsetY = static_cast<std::int32_t>(Style::spaceXs);
    std::uint32_t constraintAdjustment =
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;

    switch (area->tooltipPlacement()) {
    case TooltipPlacement::Above:
      anchorX = iconX;
      anchorY = iconY;
      anchorW = iconW;
      anchorH = 1.0f;
      anchor = XDG_POSITIONER_ANCHOR_TOP;
      gravity = XDG_POSITIONER_GRAVITY_TOP;
      offsetY = -gap;
      constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
      break;
    case TooltipPlacement::Below:
      anchorX = iconX;
      anchorY = iconY + iconH;
      anchorW = iconW;
      anchorH = 1.0f;
      anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
      gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
      offsetY = gap;
      constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
      break;
    case TooltipPlacement::Left:
      anchorX = iconX;
      anchorY = iconY;
      anchorW = 1.0f;
      anchorH = iconH;
      anchor = XDG_POSITIONER_ANCHOR_LEFT;
      gravity = XDG_POSITIONER_GRAVITY_LEFT;
      offsetX = -gap;
      constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
      break;
    case TooltipPlacement::Right:
      anchorX = iconX + iconW;
      anchorY = iconY;
      anchorW = 1.0f;
      anchorH = iconH;
      anchor = XDG_POSITIONER_ANCHOR_RIGHT;
      gravity = XDG_POSITIONER_GRAVITY_RIGHT;
      offsetX = gap;
      constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
      break;
    case TooltipPlacement::Default:
      break;
    }

    PopupSurfaceConfig config{};
    config.anchorX = static_cast<std::int32_t>(std::round(anchorX));
    config.anchorY = static_cast<std::int32_t>(std::round(anchorY));
    config.anchorWidth = std::max(1, static_cast<std::int32_t>(std::round(anchorW)));
    config.anchorHeight = std::max(1, static_cast<std::int32_t>(std::round(anchorH)));
    config.anchor = anchor;
    config.gravity = gravity;
    config.constraintAdjustment = constraintAdjustment;
    config.offsetX = offsetX;
    config.offsetY = offsetY;
    return config;
  }

} // namespace

TooltipManager& TooltipManager::instance() {
  static TooltipManager inst;
  return inst;
}

void TooltipManager::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
}

void TooltipManager::forceDestroy() {
  m_showTimer.stop();
  m_refreshTimer.stop();
  m_pendingArea = nullptr;
  m_pendingContent = {};
  m_pendingLayerParent = nullptr;
  m_pendingXdgParent = nullptr;
  m_pendingOutput = nullptr;
  m_reshowQueued = false;
  m_retargetQueued = false;
  m_destroyScheduled = false;
  m_showAfterDestroy = false;
  if (m_surface != nullptr) {
    m_surface->setDismissedCallback(nullptr);
    m_surface->setSceneRoot(nullptr);
  }
  destroyPopup();
}

void TooltipManager::shutdown() {
  forceDestroy();
  m_wayland = nullptr;
  m_config = nullptr;
  m_renderContext = nullptr;
}

void TooltipManager::onHoverChange(InputArea* area, zwlr_layer_surface_v1* parentLayerSurface, wl_output* output) {
  if (area != nullptr && area->hasTooltip() && parentLayerSurface != nullptr && output != nullptr) {
    m_pendingContent = area->tooltipContent();
    m_pendingLayerParent = parentLayerSurface;
    m_pendingXdgParent = nullptr;
    m_pendingOutput = output;
    handleHoverChange(area);
    return;
  }

  dismissPopup();
}

void TooltipManager::onHoverChange(InputArea* area, xdg_surface* parentXdgSurface, wl_output* output) {
  if (area != nullptr && area->hasTooltip() && parentXdgSurface != nullptr && output != nullptr) {
    m_pendingContent = area->tooltipContent();
    m_pendingLayerParent = nullptr;
    m_pendingXdgParent = parentXdgSurface;
    m_pendingOutput = output;
    handleHoverChange(area);
    return;
  }

  dismissPopup();
}

void TooltipManager::handleHoverChange(InputArea* area) {
  const bool sameArea = area == m_pendingArea;
  m_pendingArea = area;
  area->setTooltipChangedCallback([this](InputArea* changedArea) { refreshFromArea(changedArea); });

  switch (m_state) {
  case State::Idle:
    m_state = State::Pending;
    m_showTimer.start(kShowDelay, [this] { showPopup(); });
    break;
  case State::Pending:
    m_showTimer.stop();
    m_showTimer.start(kShowDelay, [this] { showPopup(); });
    break;
  case State::Showing:
    if (sameArea) {
      refreshFromArea(area);
      break;
    }
    if (canRetargetPopup()) {
      scheduleRetargetPopup();
    } else {
      scheduleReshow();
    }
    break;
  case State::FadingOut:
    if (canRetargetPopup()) {
      scheduleRetargetPopup();
    } else {
      scheduleReshow();
    }
    break;
  }
}

bool TooltipManager::canRetargetPopup() const {
  return m_surface != nullptr
      && m_activeLayerParent == m_pendingLayerParent
      && m_activeXdgParent == m_pendingXdgParent
      && m_activeOutput == m_pendingOutput;
}

void TooltipManager::scheduleRetargetPopup() {
  if (m_retargetQueued) {
    return;
  }
  m_retargetQueued = true;
  DeferredCall::callLater([this] {
    m_retargetQueued = false;
    if (m_pendingArea == nullptr) {
      return;
    }
    if (m_surface == nullptr) {
      showPopup();
      return;
    }
    if (m_fadeAnimId != 0) {
      m_animations.cancel(m_fadeAnimId);
      m_fadeAnimId = 0;
    }
    if (m_state == State::FadingOut) {
      m_state = State::Showing;
    }
    refreshPopupContent();
    scheduleProviderRefresh();
  });
}

void TooltipManager::scheduleReshow() {
  if (m_reshowQueued) {
    return;
  }
  m_reshowQueued = true;
  // Hover changes are delivered during pointer-event dispatch, so rebuilding now would
  // re-enter the Wayland event loop while a pointer event is still on the stack.
  // Defer to the next main-loop tick. m_pendingArea is cleared when the hover target
  // is lost or destroyed, so a non-null value here is a live area.
  DeferredCall::callLater([this] {
    m_reshowQueued = false;
    if (m_pendingArea == nullptr) {
      return;
    }
    if (m_surface == nullptr && m_state == State::Idle) {
      showPopup();
      return;
    }
    // Layer-shell allows only one popup per surface; let the compositor finish
    // tearing down the old popup before zwlr_layer_surface_v1_get_popup runs again.
    m_showAfterDestroy = true;
    scheduleDestroyPopup();
  });
}

void TooltipManager::syncAnchor(InputArea* area) {
  if (m_state != State::Showing || m_surface == nullptr || m_pendingArea != area) {
    return;
  }

  auto anchorConfig = buildTooltipAnchorConfig(area);
  anchorConfig.width = m_surface->width();
  anchorConfig.height = m_surface->height();
  m_surface->repositionAnchor(anchorConfig);
}

void TooltipManager::showPopup() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || (m_pendingLayerParent == nullptr && m_pendingXdgParent == nullptr)
      || m_pendingOutput == nullptr
      || m_pendingArea == nullptr) {
    m_state = State::Idle;
    return;
  }

  if (m_surface != nullptr) {
    m_showAfterDestroy = true;
    scheduleDestroyPopup();
    return;
  }

  m_pendingContent = m_pendingArea->tooltipContent();
  const auto [contentW, contentH] = measureContent(m_pendingContent);
  if (contentW == 0 || contentH == 0) {
    m_state = State::Idle;
    return;
  }

  auto config = buildTooltipAnchorConfig(m_pendingArea);
  config.width = contentW;
  config.height = contentH;
  config.grab = false;

  m_surface = std::make_unique<PopupSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setDismissedCallback([this] { scheduleDestroyPopup(); });

  const bool initialized = m_pendingXdgParent != nullptr
      ? m_surface->initializeAsChild(m_pendingXdgParent, m_pendingOutput, config)
      : m_surface->initialize(m_pendingLayerParent, m_pendingOutput, config);
  if (!initialized) {
    kLog.warn("failed to create tooltip popup");
    m_surface.reset();
    m_state = State::Idle;
    return;
  }

  m_renderContext->syncContentScale(m_surface->renderTarget());
  const auto [scaledContentW, scaledContentH] = measureContent(m_pendingContent);
  if (scaledContentW == 0 || scaledContentH == 0) {
    destroyPopup();
    return;
  }
  if (scaledContentW != contentW || scaledContentH != contentH) {
    m_surface->resize(scaledContentW, scaledContentH);
  }

  m_surface->setInputRegion({});
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool u, bool l) { prepareFrame(u, l); });

  m_paletteConn = paletteChanged().connect([this] {
    if (m_surface != nullptr) {
      m_surface->requestRedraw();
    }
  });

  m_state = State::Showing;
  m_activeLayerParent = m_pendingLayerParent;
  m_activeXdgParent = m_pendingXdgParent;
  m_activeOutput = m_pendingOutput;
  scheduleProviderRefresh();
  m_surface->requestUpdate();
}

void TooltipManager::dismissPopup() {
  m_refreshTimer.stop();
  m_reshowQueued = false;
  m_retargetQueued = false;
  m_showAfterDestroy = false;
  // The hover target is gone (pointer left, area lost its tooltip, or the area was
  // destroyed). Clear it so a deferred reshow does not dereference a stale area.
  m_pendingArea = nullptr;
  switch (m_state) {
  case State::Pending:
    m_showTimer.stop();
    m_state = State::Idle;
    break;
  case State::Showing:
    if (m_sceneRoot == nullptr || m_surface == nullptr) {
      scheduleDestroyPopup();
      return;
    }
    m_state = State::FadingOut;
    if (m_fadeAnimId != 0) {
      m_animations.cancel(m_fadeAnimId);
    }
    m_fadeAnimId = m_animations.animate(
        m_sceneRoot->opacity(), 0.0f, Style::animFast, Easing::EaseOutQuad,
        [this](float v) {
          if (m_sceneRoot != nullptr) {
            m_sceneRoot->setOpacity(v);
            m_sceneRoot->markPaintDirty();
          }
        },
        [this] {
          m_fadeAnimId = 0;
          scheduleDestroyPopup();
        },
        this
    );
    if (m_surface != nullptr) {
      m_surface->requestRedraw();
    }
    break;
  case State::FadingOut:
  case State::Idle:
    break;
  }
}

void TooltipManager::scheduleDestroyPopup() {
  if (m_destroyScheduled) {
    return;
  }
  if (m_state == State::Idle && m_surface == nullptr) {
    return;
  }
  m_destroyScheduled = true;
  DeferredCall::callLater([this] {
    m_destroyScheduled = false;
    destroyPopup();
    if (!m_showAfterDestroy || m_pendingArea == nullptr) {
      m_showAfterDestroy = false;
      return;
    }
    m_showAfterDestroy = false;
    DeferredCall::callLater([this] {
      if (m_pendingArea != nullptr) {
        showPopup();
      }
    });
  });
}

void TooltipManager::destroyPopup() {
  m_refreshTimer.stop();
  m_animations.cancelAll();
  m_fadeAnimId = 0;
  m_paletteConn = {};
  m_sceneRoot.reset();
  if (m_surface != nullptr) {
    m_surface->setDismissedCallback(nullptr);
    m_surface->setSceneRoot(nullptr);
  }
  m_surface.reset();
  m_activeLayerParent = nullptr;
  m_activeXdgParent = nullptr;
  m_activeOutput = nullptr;
  m_state = State::Idle;
}

void TooltipManager::refreshFromArea(InputArea* area) {
  if (area == nullptr || area != m_pendingArea || !area->hovered()) {
    return;
  }

  if (!area->hasTooltip()) {
    dismissPopup();
    return;
  }

  m_pendingContent = area->tooltipContent();
  switch (m_state) {
  case State::Pending:
    break;
  case State::Showing:
    refreshPopupContent();
    scheduleProviderRefresh();
    break;
  case State::Idle:
    if (measureContent(m_pendingContent).w > 0) {
      showPopup();
    }
    break;
  case State::FadingOut:
    break;
  }
}

void TooltipManager::refreshPopupContent() {
  if (m_surface == nullptr || m_renderContext == nullptr || m_pendingArea == nullptr) {
    return;
  }

  const auto [contentW, contentH] = measureContent(m_pendingContent);
  if (contentW == 0 || contentH == 0) {
    dismissPopup();
    return;
  }

  auto anchorConfig = buildTooltipAnchorConfig(m_pendingArea);
  anchorConfig.width = contentW;
  anchorConfig.height = contentH;
  // Leave the new size and position pending on the surface. Committing them now, while the
  // previous tooltip's buffer is still attached, makes the compositor stretch that stale buffer
  // to the new geometry (visible as a corrupted, scaled-up tooltip when moving between widgets).
  // The rebuilt scene is published with the matching buffer on the next redraw.
  m_surface->resize(contentW, contentH, false);
  m_surface->repositionAnchor(anchorConfig, false);

  m_renderContext->makeCurrent(m_surface->renderTarget());
  m_sceneRoot.reset();
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(m_pendingContent, static_cast<float>(contentW), static_cast<float>(contentH), 1.0f);
  }
  m_surface->requestRedraw();
}

void TooltipManager::scheduleProviderRefresh() {
  m_refreshTimer.stop();
  if (m_state != State::Showing || m_pendingArea == nullptr) {
    return;
  }

  const auto interval = m_pendingArea->tooltipRefreshInterval();
  if (interval.count() <= 0) {
    return;
  }

  InputArea* area = m_pendingArea;
  m_refreshTimer.start(interval, [this, area]() { refreshFromArea(area); });
}

TooltipManager::Size TooltipManager::measureContent(const TooltipContent& content) {
  if (m_renderContext == nullptr) {
    return {};
  }

  const float scale = (m_config != nullptr) ? std::max(0.1f, m_config->config().accessibility.uiScale) : 1.0f;
  const float maxContentWidth = kMaxContentWidth * scale;
  const float fontSize = Style::fontSizeCaption * scale;
  const float padH = kPadH * scale;
  const float padV = kPadV * scale;
  const float tableGap = kTableGap * scale;
  const float tableColumnGap = kTableColumnGap * scale;

  if (const auto* text = std::get_if<std::string>(&content)) {
    auto metrics = m_renderContext->measureText(*text, fontSize, FontWeight::Normal, maxContentWidth, kMaxTextLines);
    auto w = static_cast<std::uint32_t>(std::ceil(metrics.width + padH * 2.0f + kBorder * 2.0f));
    auto h = static_cast<std::uint32_t>(std::ceil((metrics.bottom - metrics.top) + padV * 2.0f + kBorder * 2.0f));
    return {std::max(w, 1U), std::max(h, 1U)};
  }

  if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&content)) {
    if (rows->empty()) {
      return {};
    }
    float maxKeyW = 0.0f;
    float maxValW = 0.0f;
    float rowH = 0.0f;
    for (const auto& row : *rows) {
      auto km = m_renderContext->measureText(row.key, fontSize);
      const auto vm = m_renderContext->measureText(row.value, fontSize);
      maxKeyW = std::max(maxKeyW, km.width);
      maxValW = std::max(maxValW, vm.width);
      rowH = std::max({rowH, km.bottom - km.top, vm.bottom - vm.top});
    }
    const TableColumnWidths columns = fitTableColumns(maxKeyW, maxValW);
    float contentW = columns.key + tableColumnGap + columns.value;
    float contentH = static_cast<float>(rows->size()) * rowH + static_cast<float>(rows->size() - 1) * tableGap;
    auto w = static_cast<std::uint32_t>(std::ceil(contentW + padH * 2.0f + kBorder * 2.0f));
    auto h = static_cast<std::uint32_t>(std::ceil(contentH + padV * 2.0f + kBorder * 2.0f));
    return {std::max(w, 1U), std::max(h, 1U)};
  }

  return {};
}

void TooltipManager::buildScene(const TooltipContent& content, float w, float h, float opacity) {
  uiAssertNotRendering("TooltipManager::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  m_sceneRoot = std::make_unique<Node>();
  m_sceneRoot->setSize(w, h);
  m_sceneRoot->setOpacity(opacity);
  m_sceneRoot->setHitTestVisible(false);
  m_surface->setSceneRoot(m_sceneRoot.get());

  m_sceneRoot->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::scaledRadiusMd(),
          .width = w,
          .height = h,
          .configure = [](Box& box) { box.setBorder(colorSpecFromRole(ColorRole::Outline), kBorder); },
      })
  );

  const float scale = (m_config != nullptr) ? std::max(0.1f, m_config->config().accessibility.uiScale) : 1.0f;
  const float maxContentWidth = kMaxContentWidth * scale;
  const float fontSize = Style::fontSizeCaption * scale;
  const float padH = kPadH * scale;
  const float padV = kPadV * scale;
  const float tableGap = kTableGap * scale;
  const float tableColumnGap = kTableColumnGap * scale;

  if (const auto* text = std::get_if<std::string>(&content)) {
    auto label = ui::label({
        .text = *text,
        .fontSize = fontSize,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxWidth = maxContentWidth,
        .maxLines = kMaxTextLines,
    });
    label->measure(*m_renderContext);
    label->setPosition(padH + kBorder, padV + kBorder);
    m_sceneRoot->addChild(std::move(label));
    return;
  }

  if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&content)) {
    const float containerW = w - (padH + kBorder) * 2.0f;

    float maxKeyW = 0.0f;
    float maxValW = 0.0f;
    for (const auto& row : *rows) {
      auto km = m_renderContext->measureText(row.key, fontSize);
      const auto vm = m_renderContext->measureText(row.value, fontSize);
      maxKeyW = std::max(maxKeyW, km.width);
      maxValW = std::max(maxValW, vm.width);
    }
    const TableColumnWidths columns = fitTableColumns(maxKeyW, maxValW);

    auto container = ui::column({
        .gap = tableGap,
        .width = containerW,
        .height = h - (padV + kBorder) * 2.0f,
        .configure = [padH, padV](Flex& flex) { flex.setPosition(padH + kBorder, padV + kBorder); },
    });

    for (const auto& row : *rows) {
      auto keyLabel = ui::label({
          .text = row.key,
          .fontSize = fontSize,
          .color = colorSpecFromRole(ColorRole::Secondary),
          .maxLines = 1,
      });
      const auto km = m_renderContext->measureText(row.key, fontSize);
      if (km.width > columns.key + 0.5f) {
        keyLabel->setMaxWidth(columns.key);
      }
      keyLabel->measure(*m_renderContext);

      auto valLabel = ui::label({
          .text = row.value,
          .fontSize = fontSize,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::End,
          .ellipsize = row.valueEllipsize,
      });
      const auto vm = m_renderContext->measureText(row.value, fontSize);
      if (vm.width > columns.value + 0.5f) {
        valLabel->setMaxWidth(columns.value);
      }
      valLabel->measure(*m_renderContext);

      container->addChild(
          ui::row(
              {
                  .justify = FlexJustify::SpaceBetween,
                  .gap = tableColumnGap,
                  .widthPolicy = FlexSizePolicy::Fill,
              },
              std::move(keyLabel), std::move(valLabel)
          )
      );
    }

    container->layout(*m_renderContext);
    m_sceneRoot->addChild(std::move(container));
  }
}

void TooltipManager::prepareFrame(bool /*needsUpdate*/, bool /*needsLayout*/) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }

  const auto width = m_surface->width();
  const auto height = m_surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  if (m_sceneRoot == nullptr) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(m_pendingContent, w, h);

    if (m_fadeAnimId != 0) {
      m_animations.cancel(m_fadeAnimId);
    }
    m_fadeAnimId = m_animations.animate(
        0.0f, 1.0f, Style::animFast, Easing::EaseOutQuad,
        [this](float v) {
          if (m_sceneRoot != nullptr) {
            m_sceneRoot->setOpacity(v);
            m_sceneRoot->markPaintDirty();
          }
        },
        [this] { m_fadeAnimId = 0; }, this
    );
  }
}
