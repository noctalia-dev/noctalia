#include "shell/tooltip/tooltip_manager.h"

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
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

  constexpr Logger kLog("tooltip");

  constexpr auto kShowDelay = std::chrono::milliseconds(500);
  constexpr float kMaxContentWidth = 280.0f;
  constexpr int kMaxTextLines = 3;
  constexpr float kPadH = Style::spaceMd;
  constexpr float kPadV = Style::spaceSm;
  constexpr float kTableGap = Style::spaceXs;
  constexpr float kTableColumnGap = Style::spaceMd;
  constexpr float kBorder = Style::borderWidth;

} // namespace

TooltipManager& TooltipManager::instance() {
  static TooltipManager inst;
  return inst;
}

void TooltipManager::initialize(WaylandConnection& wayland, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_renderContext = renderContext;
}

void TooltipManager::onHoverChange(InputArea* area, zwlr_layer_surface_v1* parentLayerSurface, wl_output* output) {
  if (area != nullptr && area->hasTooltip() && parentLayerSurface != nullptr && output != nullptr) {
    m_pendingContent = area->tooltipContent();
    m_pendingParent = parentLayerSurface;
    m_pendingOutput = output;
    m_pendingArea = area;

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
    case State::FadingOut:
      destroyPopup();
      showPopup();
      break;
    }
    return;
  }

  dismissPopup();
}

void TooltipManager::showPopup() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_pendingParent == nullptr
      || m_pendingOutput == nullptr
      || m_pendingArea == nullptr) {
    m_state = State::Idle;
    return;
  }

  const auto [contentW, contentH] = measureContent(m_pendingContent);
  if (contentW == 0 || contentH == 0) {
    m_state = State::Idle;
    return;
  }

  float absX = 0.0f;
  float absY = 0.0f;
  Node::absolutePosition(m_pendingArea, absX, absY);

  auto config = PopupSurfaceConfig{
      .anchorX = static_cast<std::int32_t>(std::round(absX)),
      .anchorY = static_cast<std::int32_t>(std::round(absY)),
      .anchorWidth = std::max(1, static_cast<std::int32_t>(std::round(m_pendingArea->width()))),
      .anchorHeight = std::max(1, static_cast<std::int32_t>(std::round(m_pendingArea->height()))),
      .width = contentW,
      .height = contentH,
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .constraintAdjustment =
          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(Style::spaceXs),
      .grab = false,
  };

  m_surface = std::make_unique<PopupSurface>(*m_wayland);
  m_surface->setRenderContext(m_renderContext);
  m_surface->setDismissedCallback([this] {
    m_animations.cancelAll();
    m_fadeAnimId = 0;
    m_sceneRoot.reset();
    m_surface.reset();
    m_state = State::Idle;
  });

  if (!m_surface->initialize(m_pendingParent, m_pendingOutput, config)) {
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
  m_surface->requestUpdate();
}

void TooltipManager::dismissPopup() {
  switch (m_state) {
  case State::Pending:
    m_showTimer.stop();
    m_state = State::Idle;
    break;
  case State::Showing:
    if (m_sceneRoot == nullptr || m_surface == nullptr) {
      destroyPopup();
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
          DeferredCall::callLater([this] { destroyPopup(); });
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

void TooltipManager::destroyPopup() {
  m_animations.cancelAll();
  m_fadeAnimId = 0;
  m_paletteConn = {};
  m_sceneRoot.reset();
  m_surface.reset();
  m_state = State::Idle;
}

TooltipManager::Size TooltipManager::measureContent(const TooltipContent& content) {
  if (m_renderContext == nullptr) {
    return {};
  }

  if (const auto* text = std::get_if<std::string>(&content)) {
    auto metrics = m_renderContext->measureText(
        *text, Style::fontSizeCaption, FontWeight::Normal, kMaxContentWidth, kMaxTextLines
    );
    auto w = static_cast<std::uint32_t>(std::ceil(metrics.width + (kPadH * 2.0f) + (kBorder * 2.0f)));
    auto h = static_cast<std::uint32_t>(std::ceil((metrics.bottom - metrics.top) + (kPadV * 2.0f) + (kBorder * 2.0f)));
    return {std::max(w, 1u), std::max(h, 1u)};
  }

  if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&content)) {
    if (rows->empty()) {
      return {};
    }
    float maxKeyW = 0.0f;
    float maxValW = 0.0f;
    float rowH = 0.0f;
    for (const auto& row : *rows) {
      auto km = m_renderContext->measureText(row.key, Style::fontSizeCaption);
      const auto vm = m_renderContext->measureText(row.value, Style::fontSizeCaption);
      maxKeyW = std::max(maxKeyW, km.width);
      maxValW = std::max(maxValW, vm.width);
      rowH = std::max(rowH, std::max(km.bottom - km.top, vm.bottom - vm.top));
    }
    float naturalW = maxKeyW + kTableColumnGap + maxValW;
    float contentW = std::min(naturalW, kMaxContentWidth);
    if (naturalW > kMaxContentWidth) {
      maxValW = kMaxContentWidth - maxKeyW - kTableColumnGap;
    }
    float contentH = (static_cast<float>(rows->size()) * rowH) + (static_cast<float>(rows->size() - 1) * kTableGap);
    auto w = static_cast<std::uint32_t>(std::ceil(contentW + (kPadH * 2.0f) + (kBorder * 2.0f)));
    auto h = static_cast<std::uint32_t>(std::ceil(contentH + (kPadV * 2.0f) + (kBorder * 2.0f)));
    return {std::max(w, 1u), std::max(h, 1u)};
  }

  return {};
}

void TooltipManager::buildScene(const TooltipContent& content, float w, float h) {
  uiAssertNotRendering("TooltipManager::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  m_sceneRoot = std::make_unique<Node>();
  m_sceneRoot->setSize(w, h);
  m_sceneRoot->setOpacity(0.0f);
  m_sceneRoot->setHitTestVisible(false);
  m_surface->setSceneRoot(m_sceneRoot.get());

  m_sceneRoot->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Surface),
          .radius = Style::scaledRadiusMd(),
          .width = w,
          .height = h,
          .configure = [](Box& box) { box.setBorder(colorSpecFromRole(ColorRole::Outline, 0.5f), kBorder); },
      })
  );

  if (const auto* text = std::get_if<std::string>(&content)) {
    auto label = ui::label({
        .text = *text,
        .fontSize = Style::fontSizeCaption,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxWidth = kMaxContentWidth,
        .maxLines = kMaxTextLines,
    });
    label->measure(*m_renderContext);
    label->setPosition(kPadH + kBorder, kPadV + kBorder);
    m_sceneRoot->addChild(std::move(label));
    return;
  }

  if (const auto* rows = std::get_if<std::vector<TooltipRow>>(&content)) {
    const float containerW = w - ((kPadH + kBorder) * 2.0f);

    float maxKeyW = 0.0f;
    for (const auto& row : *rows) {
      auto km = m_renderContext->measureText(row.key, Style::fontSizeCaption);
      maxKeyW = std::max(maxKeyW, km.width);
    }
    const float valMaxW = std::max(0.0f, containerW - maxKeyW - kTableColumnGap);

    auto container = ui::column({
        .gap = kTableGap,
        .width = containerW,
        .height = h - ((kPadV + kBorder) * 2.0f),
        .configure = [](Flex& flex) { flex.setPosition(kPadH + kBorder, kPadV + kBorder); },
    });

    for (const auto& row : *rows) {
      auto keyLabel = ui::label({
          .text = row.key,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      });
      keyLabel->measure(*m_renderContext);

      auto valLabel = ui::label({
          .text = row.value,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .textAlign = TextAlign::End,
      });
      const auto vm = m_renderContext->measureText(row.value, Style::fontSizeCaption);
      if (vm.width > valMaxW + 0.5f) {
        valLabel->setMaxWidth(valMaxW);
      }
      valLabel->measure(*m_renderContext);

      container->addChild(
          ui::row(
              {
                  .justify = FlexJustify::SpaceBetween,
                  .gap = kTableColumnGap,
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

  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);

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
