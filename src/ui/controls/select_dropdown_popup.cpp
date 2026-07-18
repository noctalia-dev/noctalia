#include "ui/controls/select_dropdown_popup.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/box.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("select-dropdown-popup");
  constexpr float kMenuPadding = Style::spaceXs;

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

SelectDropdownPopup::SelectDropdownPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext) {}

SelectDropdownPopup::~SelectDropdownPopup() { closeSelectDropdown(); }

void SelectDropdownPopup::setParent(
    zwlr_layer_surface_v1* layerSurface, wl_surface* parentWlSurface, wl_output* output
) {
  m_parentLayerSurface = layerSurface;
  m_parentXdgSurface = nullptr;
  m_parentWlSurface = parentWlSurface;
  m_parentOutput = output;
}

void SelectDropdownPopup::setParent(xdg_surface* xdgSurface, wl_surface* parentWlSurface, wl_output* output) {
  m_parentLayerSurface = nullptr;
  m_parentXdgSurface = xdgSurface;
  m_parentWlSurface = parentWlSurface;
  m_parentOutput = output;
}

void SelectDropdownPopup::setShadowConfig(const ShellConfig::ShadowConfig& shadow) {
  if (m_shadowConfig == shadow) {
    return;
  }
  m_shadowConfig = shadow;
  if (isSelectDropdownOpen()) {
    closeSelectDropdown();
  }
}

void SelectDropdownPopup::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
  if (m_openInProgress) {
    m_closeRequestedDuringOpen = true;
    if (callbacks.onDismiss) {
      callbacks.onDismiss();
    }
    return;
  }

  closeSelectDropdown();

  if (m_parentLayerSurface == nullptr && m_parentXdgSurface == nullptr) {
    kLog.warn("no parent surface set");
    return;
  }

  m_closeRequestedDuringOpen = false;
  m_callbacks = std::move(callbacks);
  m_options = request.options;
  m_selectedIndex = request.selectedIndex;
  m_hoveredIndex = m_selectedIndex < m_options.size() ? m_selectedIndex : 0;
  m_optionHeight = request.optionHeight;
  m_menuWidth = request.menuWidth;

  const std::size_t visibleCount = std::min(request.maxVisibleOptions, m_options.size());
  m_viewportHeight = static_cast<float>(visibleCount) * m_optionHeight + kMenuPadding * 2.0f;
  m_totalHeight = static_cast<float>(m_options.size()) * m_optionHeight;
  m_scrollOffset = 0.0f;
  const auto chrome =
      popup_chrome::computeGeometry(m_menuWidth, m_viewportHeight, m_shadowConfig, Style::popupShadowsEnabled());

  ensureHoveredIndexVisible();

  PopupSurfaceConfig popupCfg{
      .anchorX = request.anchorX,
      .anchorY = request.anchorY,
      .anchorWidth = std::max(1, request.anchorWidth),
      .anchorHeight = std::max(1, request.anchorHeight),
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(std::lround(Style::spaceXs)),
      .serial = m_wayland.lastInputSerial(),
      .grab = true,
  };
  popup_chrome::applyToConfig(
      popupCfg, chrome,
      popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      }
  );

  m_surface = std::make_unique<PopupSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);

  auto* self = this;

  m_surface->setConfigureCallback([self](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    self->m_surface->requestLayout();
  });

  m_surface->setPrepareFrameCallback([self, request](bool /*needsUpdate*/, bool needsLayout) {
    if (self->m_surface == nullptr) {
      return;
    }

    const auto width = self->m_surface->width();
    const auto height = self->m_surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    self->m_renderContext.makeCurrent(self->m_surface->renderTarget());

    const bool needsSceneBuild = self->m_sceneDirty
        || self->m_sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (needsSceneBuild) {
      self->m_sceneDirty = false;
      self->buildScene(request);
    }

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);
    self->m_sceneRoot->setSize(fw, fh);
    self->m_sceneRoot->layout(self->m_renderContext);

    self->m_inputDispatcher.setSceneRoot(self->m_sceneRoot.get());
    self->m_inputDispatcher.setCursorShapeCallback([self](std::uint32_t serial, std::uint32_t shape) {
      self->m_wayland.setCursorShape(serial, shape);
    });
    self->m_surface->setSceneRoot(self->m_sceneRoot.get());
  });

  m_surface->setDismissedCallback([self]() { DeferredCall::callLater([self]() { self->closeSelectDropdown(); }); });

  m_openInProgress = true;
  const bool initialized = m_parentLayerSurface != nullptr
      ? m_surface->initialize(m_parentLayerSurface, m_parentOutput, popupCfg)
      : m_surface->initializeAsChild(m_parentXdgSurface, m_parentOutput, popupCfg);
  m_openInProgress = false;
  if (!initialized) {
    kLog.warn("failed to create select dropdown popup");
    closeSelectDropdown();
    return;
  }
  if (m_closeRequestedDuringOpen) {
    closeSelectDropdown();
    return;
  }

  popup_chrome::setContentInputRegion(*m_surface, chrome);

  m_wlSurface = m_surface->wlSurface();
  syncPointerStateFromCurrentPosition();
}

void SelectDropdownPopup::closeSelectDropdown() {
  auto onDismiss = std::move(m_callbacks.onDismiss);
  m_optionViews.clear();
  m_inputDispatcher.setSceneRoot(nullptr);
  m_sceneRoot.reset();
  if (m_openInProgress) {
    m_closeRequestedDuringOpen = true;
  } else {
    m_surface.reset();
    m_closeRequestedDuringOpen = false;
  }
  m_wlSurface = nullptr;
  m_pointerInside = false;
  m_pointerOnSurface = false;
  m_contentNode = nullptr;
  m_scrollbar = nullptr;
  m_options.clear();
  m_callbacks = {};
  m_sceneDirty = false;
  if (onDismiss) {
    onDismiss();
  }
}

bool SelectDropdownPopup::isSelectDropdownOpen() const { return m_surface != nullptr; }

void SelectDropdownPopup::buildScene(const DropdownRequest& request) {
  m_sceneRoot = std::make_unique<Node>();
  m_optionViews.clear();
  const auto chrome =
      popup_chrome::computeGeometry(m_menuWidth, m_viewportHeight, m_shadowConfig, Style::popupShadowsEnabled());
  const float menuX = chrome.contentX();
  const float menuY = chrome.contentY();
  const float radius = Style::scaledRadiusMd();

  if (Style::popupShadowsEnabled()) {
    (void)popup_chrome::addShadow(*m_sceneRoot, chrome, m_shadowConfig, radius);
  }

  auto bg = std::make_unique<RectNode>();
  bg->setStyle(
      RoundedRectStyle{
          .fill = resolved(ColorRole::SurfaceVariant),
          .border = resolved(ColorRole::Outline),
          .fillMode = FillMode::Solid,
          .radius = radius,
          .softness = 1.0f,
          .borderWidth = Style::popupBordersEnabled() ? Style::borderWidth : 0.0f,
      }
  );
  auto* bgNode = static_cast<RectNode*>(m_sceneRoot->addChild(std::move(bg)));
  bgNode->setPosition(menuX, menuY);
  bgNode->setFrameSize(m_menuWidth, m_viewportHeight);

  const float contentViewport = m_viewportHeight - kMenuPadding * 2.0f;
  const bool scrollable = m_totalHeight > contentViewport + 0.5f;
  const float scrollbarGutter = scrollable ? Style::scrollbarWidth : 0.0f;
  const float rowWidth = m_menuWidth - kMenuPadding * 2.0f - scrollbarGutter;

  auto viewport = std::make_unique<Node>();
  viewport->setClipChildren(true);
  viewport->setPosition(menuX, menuY + kMenuPadding);
  viewport->setFrameSize(m_menuWidth, contentViewport);
  auto* viewportNode = m_sceneRoot->addChild(std::move(viewport));

  auto content = std::make_unique<Node>();
  content->setPosition(kMenuPadding, -m_scrollOffset);
  content->setFrameSize(rowWidth, m_totalHeight);
  m_contentNode = viewportNode->addChild(std::move(content));

  const bool hasIndicators = !request.indicatorColors.empty();
  const float indicatorSize = hasIndicators ? std::round(request.fontSize) : 0.0f;
  const float indicatorBorder = hasIndicators ? 1.5f : 0.0f;

  for (std::size_t i = 0; i < m_options.size(); ++i) {
    const float rowY = static_cast<float>(i) * m_optionHeight;

    auto rowBg = std::make_unique<RectNode>();
    rowBg->setPosition(0.0f, rowY);
    rowBg->setFrameSize(rowWidth, m_optionHeight);
    auto* rowBgPtr = static_cast<RectNode*>(m_contentNode->addChild(std::move(rowBg)));

    const bool hasPreview = i < request.optionSwatchPreviews.size() && !request.optionSwatchPreviews[i].empty();
    float leadingInset = 0.0f;
    if (hasPreview) {
      auto preview = std::make_unique<ColorSwatchPreviewStrip>();
      preview->setMetricsFromFontSize(request.fontSize);
      preview->setPreview(request.optionSwatchPreviews[i]);
      const float previewY = rowY + std::round((m_optionHeight - preview->preferredHeight()) * 0.5f);
      preview->setPosition(request.horizontalPadding, previewY);
      leadingInset = preview->preferredWidth() + Style::spaceSm;
      m_contentNode->addChild(std::move(preview));
    } else if (hasIndicators && i < request.indicatorColors.size()) {
      auto indicator = std::make_unique<Box>();
      indicator->setFill(request.indicatorColors[i]);
      indicator->setBorder(colorSpecFromRole(ColorRole::Outline), indicatorBorder);
      indicator->setFrameSize(indicatorSize, indicatorSize);
      indicator->setRadius(indicatorSize * 0.5f);
      indicator->setPosition(request.horizontalPadding, rowY + std::round((m_optionHeight - indicatorSize) * 0.5f));
      m_contentNode->addChild(std::move(indicator));
      leadingInset = indicatorSize + Style::spaceSm;
    }

    auto label = std::make_unique<Label>();
    label->setText(m_options[i]);
    label->setFontSize(request.fontSize);
    label->setMaxLines(1);
    const float labelLeft = request.horizontalPadding + leadingInset;
    label->setMaxWidth(
        std::max(0.0f, rowWidth - labelLeft - request.horizontalPadding - request.glyphSize - Style::spaceXs)
    );
    label->measure(m_renderContext);
    float labelY = std::round((m_optionHeight - label->height()) * 0.5f);
    label->setPosition(labelLeft, rowY + labelY);
    auto* labelPtr = static_cast<Label*>(m_contentNode->addChild(std::move(label)));

    Glyph* checkPtr = nullptr;
    if (i == m_selectedIndex) {
      auto checkGlyph = std::make_unique<Glyph>();
      checkGlyph->setGlyph("check");
      checkGlyph->setGlyphSize(request.glyphSize);
      checkGlyph->measure(m_renderContext);
      float glyphY = std::round((m_optionHeight - checkGlyph->height()) * 0.5f);
      checkGlyph->setPosition(rowWidth - request.horizontalPadding - checkGlyph->width(), rowY + glyphY);
      checkPtr = static_cast<Glyph*>(m_contentNode->addChild(std::move(checkGlyph)));
    }

    m_optionViews.push_back(OptionView{.background = rowBgPtr, .label = labelPtr, .checkGlyph = checkPtr});

    auto area = std::make_unique<InputArea>();
    area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    area->setPosition(0.0f, rowY);
    area->setFrameSize(rowWidth, m_optionHeight);
    area->setOnEnter([this, i](const InputArea::PointerData& /*data*/) {
      m_hoveredIndex = i;
      applyHoverVisuals();
      if (m_surface) {
        m_surface->requestRedraw();
      }
    });
    area->setOnClick([this, i](const InputArea::PointerData& data) {
      if (data.button != BTN_LEFT) {
        return;
      }
      selectAndClose(i);
    });
    area->setOnAxisHandler([this](const InputArea::PointerData& data) {
      if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return false;
      }
      scrollBy(data.scrollDelta(m_optionHeight));
      return true;
    });
    m_contentNode->addChild(std::move(area));
  }

  auto scrollbar = std::make_unique<Scrollbar>();
  scrollbar->setOnScrollChanged([this](float offset) { setScrollOffset(offset); });
  const float scrollbarX =
      menuX + m_menuWidth - Style::scrollbarWidth - (Style::popupBordersEnabled() ? Style::borderWidth : 0.0f);
  scrollbar->setPosition(scrollbarX, menuY + kMenuPadding);
  scrollbar->update(contentViewport, m_totalHeight, m_scrollOffset);
  m_scrollbar = static_cast<Scrollbar*>(m_sceneRoot->addChild(std::move(scrollbar)));

  applyHoverVisuals();
}

void SelectDropdownPopup::selectAndClose(std::size_t index) {
  auto onSelect = m_callbacks.onSelect;
  m_callbacks.onDismiss = nullptr;
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, onSelect, index]() {
    if (aliveGuard.expired()) {
      return;
    }
    const bool hadSurface = m_surface != nullptr;
    closeSelectDropdown();
    // xdg_popup children must attach to the compositor's topmost popup. Flush and roundtrip so
    // the dropdown destroy is processed before any follow-up dialog opens on the parent chain.
    if (hadSurface) {
      wl_display_flush(m_wayland.display());
      if (wl_display_roundtrip(m_wayland.display()) < 0) {
        kLog.warn("select dropdown: post-close roundtrip failed (compositor protocol error)");
      }
    }
    if (onSelect) {
      onSelect(index);
    }
  });
}

void SelectDropdownPopup::scrollBy(float delta) { setScrollOffset(m_scrollOffset + delta); }

void SelectDropdownPopup::setScrollOffset(float offset) {
  const float contentViewport = m_viewportHeight - kMenuPadding * 2.0f;
  const float maxScroll = std::max(0.0f, m_totalHeight - contentViewport);
  m_scrollOffset = std::clamp(offset, 0.0f, maxScroll);
  applyScrollOffset();
  if (m_surface) {
    m_surface->requestRedraw();
  }
}

void SelectDropdownPopup::applyScrollOffset() {
  if (m_contentNode != nullptr) {
    m_contentNode->setPosition(kMenuPadding, -m_scrollOffset);
  }
  if (m_scrollbar != nullptr) {
    const float contentViewport = m_viewportHeight - kMenuPadding * 2.0f;
    m_scrollbar->update(contentViewport, m_totalHeight, m_scrollOffset);
  }
}

void SelectDropdownPopup::invalidateScene() {
  m_sceneDirty = true;
  if (m_surface) {
    m_surface->requestLayout();
  }
}

void SelectDropdownPopup::applyHoverVisuals() {
  for (std::size_t i = 0; i < m_optionViews.size(); ++i) {
    auto& view = m_optionViews[i];
    const bool isHovered = (i == m_hoveredIndex);
    const Color bgColor = isHovered ? resolved(ColorRole::Hover) : clearColor();
    const ColorSpec fg = isHovered ? colorSpecFromRole(ColorRole::OnHover) : colorSpecFromRole(ColorRole::OnSurface);

    if (view.background != nullptr) {
      view.background->setStyle(
          RoundedRectStyle{
              .fill = bgColor,
              .border = bgColor,
              .fillMode = FillMode::Solid,
              .radius = Style::scaledRadiusSm(),
              .softness = 1.0f,
              .borderWidth = 0.0f,
          }
      );
    }
    if (view.label != nullptr) {
      view.label->setColor(fg);
    }
    if (view.checkGlyph != nullptr) {
      view.checkGlyph->setColor(fg);
    }
  }
}

void SelectDropdownPopup::clampScrollOffset() {
  const float contentViewport = m_viewportHeight - kMenuPadding * 2.0f;
  const float maxScroll = std::max(0.0f, m_totalHeight - contentViewport);
  m_scrollOffset = std::clamp(m_scrollOffset, 0.0f, maxScroll);
}

void SelectDropdownPopup::ensureHoveredIndexVisible() {
  if (m_options.empty() || m_hoveredIndex >= m_options.size()) {
    return;
  }

  const float contentViewport = m_viewportHeight - kMenuPadding * 2.0f;
  const float itemTop = static_cast<float>(m_hoveredIndex) * m_optionHeight;
  const float itemBottom = itemTop + m_optionHeight;

  if (itemTop < m_scrollOffset) {
    setScrollOffset(itemTop);
  } else if (itemBottom > m_scrollOffset + contentViewport) {
    setScrollOffset(itemBottom - contentViewport);
  }
}

bool SelectDropdownPopup::onPointerEvent(const PointerEvent& event) {
  if (!isSelectDropdownOpen()) {
    return false;
  }

  const bool captured = m_inputDispatcher.pointerCaptured();
  float localX = 0.0f;
  float localY = 0.0f;
  const bool mapped = mapPointerEvent(event, localX, localY);
  if (!mapped) {
    if ((event.type == PointerEvent::Type::Leave && event.surface == m_parentWlSurface)
        || (event.type == PointerEvent::Type::Motion && event.surface == m_parentWlSurface && m_pointerInside)) {
      m_pointerInside = false;
      m_pointerOnSurface = false;
      if (!captured) {
        m_inputDispatcher.pointerLeave();
      }
    }
    if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
      m_surface->requestRedraw();
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    m_pointerOnSurface = ownsSurface(resolveEventSurface(event));
    m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    m_pointerOnSurface = false;
    if (!captured) {
      m_inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    m_pointerOnSurface = ownsSurface(resolveEventSurface(event));
    if (captured) {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    } else if (!m_pointerInside) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    }
    return true;
  case PointerEvent::Type::Button:
    m_pointerOnSurface = ownsSurface(resolveEventSurface(event));
    if (captured) {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    } else if (!m_pointerInside) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    }
    {
      const bool pressed = (event.state == 1);
      m_inputDispatcher.pointerButton(localX, localY, event.button, pressed);
      if (!pressed && captured && (!m_pointerInside || !containsPopupContent(localX, localY))) {
        m_pointerInside = false;
        m_pointerOnSurface = false;
        m_inputDispatcher.pointerLeave();
      }
    }
    return true;
  case PointerEvent::Type::Axis:
    m_pointerOnSurface = ownsSurface(resolveEventSurface(event));
    if (captured) {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    } else if (!m_pointerInside) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    }
    m_inputDispatcher.pointerAxis(
        localX, localY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
        event.axisLines
    );
    return true;
  }

  if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
    m_surface->requestRedraw();
  }

  return true;
}

void SelectDropdownPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isSelectDropdownOpen()) {
    return;
  }
  handleKey(event.sym, event.utf32, event.modifiers, event.pressed);
}

void SelectDropdownPopup::handleKey(std::uint32_t sym, std::uint32_t /*utf32*/, std::uint32_t modifiers, bool pressed) {
  if (!pressed) {
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    auto onDismiss = m_callbacks.onDismiss;
    const std::weak_ptr<void> aliveGuard = m_aliveGuard;
    DeferredCall::callLater([this, aliveGuard, onDismiss]() {
      if (aliveGuard.expired()) {
        return;
      }
      closeSelectDropdown();
      if (onDismiss) {
        onDismiss();
      }
    });
  } else if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (!m_options.empty()) {
      m_hoveredIndex = (m_hoveredIndex + 1) % m_options.size();
      ensureHoveredIndexVisible();
      applyHoverVisuals();
      if (m_surface) {
        m_surface->requestRedraw();
      }
    }
  } else if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (!m_options.empty()) {
      m_hoveredIndex = (m_hoveredIndex + m_options.size() - 1) % m_options.size();
      ensureHoveredIndexVisible();
      applyHoverVisuals();
      if (m_surface) {
        m_surface->requestRedraw();
      }
    }
  } else if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (m_hoveredIndex < m_options.size()) {
      selectAndClose(m_hoveredIndex);
    }
  } else if (KeySymbol::isHome(sym)) {
    m_hoveredIndex = 0;
    ensureHoveredIndexVisible();
    applyHoverVisuals();
    if (m_surface) {
      m_surface->requestRedraw();
    }
  } else if (KeySymbol::isEnd(sym)) {
    if (!m_options.empty()) {
      m_hoveredIndex = m_options.size() - 1;
      ensureHoveredIndexVisible();
      applyHoverVisuals();
      if (m_surface) {
        m_surface->requestRedraw();
      }
    }
  }
}

wl_surface* SelectDropdownPopup::wlSurface() const noexcept { return m_wlSurface; }

xdg_surface* SelectDropdownPopup::xdgSurface() const noexcept {
  return m_surface != nullptr ? m_surface->xdgSurface() : nullptr;
}

std::uint32_t SelectDropdownPopup::popupWidth() const noexcept { return m_surface != nullptr ? m_surface->width() : 0; }

std::uint32_t SelectDropdownPopup::popupHeight() const noexcept {
  return m_surface != nullptr ? m_surface->height() : 0;
}

bool SelectDropdownPopup::mapPointerEvent(const PointerEvent& event, float& localX, float& localY) const noexcept {
  if (m_surface == nullptr) {
    return false;
  }

  wl_surface* eventSurface = resolveEventSurface(event);
  if (eventSurface == nullptr) {
    return false;
  }

  if (m_inputDispatcher.pointerCaptured() && event.type != PointerEvent::Type::Leave) {
    if (ownsSurface(eventSurface)) {
      localX = static_cast<float>(event.sx);
      localY = static_cast<float>(event.sy);
      return true;
    }
    if (eventSurface == m_parentWlSurface) {
      localX = static_cast<float>(event.sx) - static_cast<float>(m_surface->configuredX());
      localY = static_cast<float>(event.sy) - static_cast<float>(m_surface->configuredY());
      return true;
    }
  }

  if (ownsSurface(eventSurface)) {
    localX = static_cast<float>(event.sx);
    localY = static_cast<float>(event.sy);
    return true;
  }

  if (eventSurface != m_parentWlSurface) {
    return false;
  }

  localX = static_cast<float>(event.sx) - static_cast<float>(m_surface->configuredX());
  localY = static_cast<float>(event.sy) - static_cast<float>(m_surface->configuredY());

  if (event.type == PointerEvent::Type::Leave) {
    return m_pointerInside || m_inputDispatcher.pointerCaptured();
  }

  if (m_inputDispatcher.pointerCaptured()) {
    return true;
  }
  if (event.type == PointerEvent::Type::Button && m_pointerInside) {
    return true;
  }
  return containsPopupContent(localX, localY);
}

wl_surface* SelectDropdownPopup::resolveEventSurface(const PointerEvent& event) const noexcept {
  wl_surface* eventSurface = event.surface;
  if (eventSurface == nullptr && event.type == PointerEvent::Type::Motion) {
    eventSurface = m_wayland.lastPointerSurface();
  }
  return eventSurface;
}

void SelectDropdownPopup::syncPointerStateFromCurrentPosition() {
  if (m_surface == nullptr || !m_wayland.hasPointerPosition()) {
    return;
  }

  PointerEvent synthetic;
  synthetic.type = PointerEvent::Type::Motion;
  synthetic.surface = m_wayland.lastPointerSurface();
  synthetic.sx = m_wayland.lastPointerX();
  synthetic.sy = m_wayland.lastPointerY();
  synthetic.serial = m_wayland.lastInputSerial();

  float localX = 0.0f;
  float localY = 0.0f;
  if (!mapPointerEvent(synthetic, localX, localY)) {
    return;
  }

  m_pointerInside = true;
  m_pointerOnSurface = ownsSurface(synthetic.surface);
  m_inputDispatcher.pointerEnter(localX, localY, synthetic.serial);
  if (m_surface->isRunning()) {
    m_surface->requestRedraw();
  }
}

bool SelectDropdownPopup::ownsSurface(wl_surface* surface) const noexcept {
  return m_surface != nullptr && surface != nullptr && surface == m_surface->wlSurface();
}

bool SelectDropdownPopup::containsPopupContent(float localX, float localY) const noexcept {
  const auto chrome =
      popup_chrome::computeGeometry(m_menuWidth, m_viewportHeight, m_shadowConfig, Style::popupShadowsEnabled());
  return localX >= chrome.contentX()
      && localY >= chrome.contentY()
      && localX < chrome.contentRight()
      && localY < chrome.contentBottom();
}
