#include "ui/controls/context_menu_popup.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("context-menu-popup");

} // namespace

ContextMenuPopup* ContextMenuPopup::s_openMenu = nullptr;

ContextMenuPopup::ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext) {}

ContextMenuPopup::~ContextMenuPopup() { close(); }

void ContextMenuPopup::open(ContextMenuPopupRequest request) {
  close();

  // maxVisible caps the popup viewport; all entries remain reachable via scroll.
  const std::size_t maxVisible =
      request.maxVisible > 0 ? request.maxVisible : std::max<std::size_t>(1, request.entries.size());
  const float menuHeight = ContextMenuControl::preferredHeight(request.entries, maxVisible);
  const auto chrome =
      popup_chrome::computeGeometry(request.menuWidth, menuHeight, m_shadowConfig, Style::popupShadowsEnabled());
  m_scrollState = {};
  m_scrollView = nullptr;
  m_menu = nullptr;
  m_highlightedIndex = 0;

  const ContextMenuPopupPlacement defaultPlacement{
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(Style::spaceXs),
      .chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      },
  };
  const ContextMenuPopupPlacement resolvedPlacement = request.placement.value_or(defaultPlacement);

  PopupSurfaceConfig popupCfg{
      .anchorX = request.anchor.x,
      .anchorY = request.anchor.y,
      .anchorWidth = std::max(1, request.anchor.width),
      .anchorHeight = std::max(1, request.anchor.height),
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = resolvedPlacement.anchor,
      .gravity = resolvedPlacement.gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = resolvedPlacement.offsetX,
      .offsetY = resolvedPlacement.offsetY,
      .serial = m_wayland.lastInputSerial(),
      .grab = true,
  };
  popup_chrome::applyToConfig(popupCfg, chrome, resolvedPlacement.chromeAttachment);

  m_surface = std::make_unique<PopupSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);

  auto* self = this;

  m_surface->setConfigureCallback([self](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    self->m_surface->requestLayout();
  });

  m_surface->setPrepareFrameCallback([self, entries = std::move(request.entries),
                                      chrome](bool /*needsUpdate*/, bool needsLayout) {
    if (self->m_surface == nullptr) {
      return;
    }

    const auto width = self->m_surface->width();
    const auto height = self->m_surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    self->m_renderContext.makeCurrent(self->m_surface->renderTarget());

    const bool needsSceneBuild = self->m_sceneRoot == nullptr
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->width())) != width
        || static_cast<std::uint32_t>(std::round(self->m_sceneRoot->height())) != height;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (self->m_menu != nullptr) {
      self->m_highlightedIndex = self->m_menu->highlightedIndex();
    }

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);

    self->m_sceneRoot = std::make_unique<Node>();
    self->m_sceneRoot->setSize(fw, fh);
    if (Style::popupShadowsEnabled()) {
      (void)popup_chrome::addShadow(*self->m_sceneRoot, chrome, self->m_shadowConfig, Style::scaledRadiusLg());
    }

    auto scrollView = std::make_unique<ScrollView>();
    scrollView->setPosition(chrome.contentX(), chrome.contentY());
    scrollView->setSize(chrome.contentWidth, chrome.contentHeight);
    scrollView->setViewportPaddingH(0.0f);
    scrollView->setViewportPaddingV(0.0f);
    scrollView->clearFill();
    scrollView->clearBorder();
    scrollView->setRadius(0.0f);
    scrollView->bindState(&self->m_scrollState);
    scrollView->setScrollbarVisible(true);

    auto ctrl = std::make_unique<ContextMenuControl>();
    ContextMenuControl* menuPtr = ctrl.get();
    ctrl->setMenuWidth(chrome.contentWidth);
    // Lay out every entry; ScrollView clips to maxVisible viewport height.
    ctrl->setMaxVisible(entries.size());
    ctrl->setEntries(entries);
    ctrl->setHighlightedIndex(self->m_highlightedIndex);
    self->m_highlightedIndex = ctrl->highlightedIndex();
    ctrl->setRedrawCallback([self]() {
      if (self->m_surface)
        self->m_surface->requestRedraw();
    });
    ctrl->setOnActivate([self](const ContextMenuControlEntry& e) {
      auto onActivate = self->m_onActivate;
      DeferredCall::callLater([self, onActivate, e]() {
        if (onActivate) {
          onActivate(e);
        }
        self->close();
      });
    });
    scrollView->content()->addChild(std::move(ctrl));
    ScrollView* scrollPtr = scrollView.get();
    scrollView->layout(self->m_renderContext);

    self->m_scrollView = scrollPtr;
    self->m_menu = menuPtr;
    self->ensureHighlightedVisible();

    self->m_sceneRoot->addChild(std::move(scrollView));
    self->m_inputDispatcher.setSceneRoot(self->m_sceneRoot.get());
    self->m_inputDispatcher.setCursorShapeCallback([self](std::uint32_t serial, std::uint32_t shape) {
      self->m_wayland.setCursorShape(serial, shape);
    });
    self->m_surface->setSceneRoot(self->m_sceneRoot.get());
  });

  m_surface->setDismissedCallback([self]() { DeferredCall::callLater([self]() { self->close(); }); });

  // Layer-shell popups inherit their parent's keyboard interactivity. A bar is
  // None, so flip it to OnDemand before the popup maps or the grabbing popup
  // gets no keyboard focus and ESC cannot reach it. Only bar-style callers pass
  // a parent wlSurface; panels are already OnDemand and leave it null.
  if (request.parent.layerSurface != nullptr && request.parent.wlSurface != nullptr) {
    m_keyboardParentLayerSurface = request.parent.layerSurface;
    m_keyboardParentWlSurface = request.parent.wlSurface;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        request.parent.layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::OnDemand)
    );
    wl_surface_commit(request.parent.wlSurface);
  }

  const bool initialized = request.parent.xdgSurface != nullptr
      ? m_surface->initializeAsChild(request.parent.xdgSurface, request.parent.output, popupCfg)
      : m_surface->initialize(request.parent.layerSurface, request.parent.output, popupCfg);
  if (!initialized) {
    kLog.warn("failed to create context menu popup");
    restoreParentKeyboardInteractivity();
    m_surface.reset();
    return;
  }

  popup_chrome::setContentInputRegion(*m_surface, chrome);
  m_wlSurface = m_surface->wlSurface();
  s_openMenu = this;
}

void ContextMenuPopup::close() {
  const bool wasOpen = m_surface != nullptr;
  if (s_openMenu == this) {
    s_openMenu = nullptr;
  }
  restoreParentKeyboardInteractivity();
  m_menu = nullptr;
  m_scrollView = nullptr;
  m_sceneRoot.reset();
  m_surface.reset();
  m_wlSurface = nullptr;
  m_pointerInside = false;
  if (wasOpen && m_onDismissed) {
    m_onDismissed();
  }
}

bool ContextMenuPopup::isOpen() const noexcept { return m_surface != nullptr; }

void ContextMenuPopup::setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback) {
  m_onActivate = std::move(callback);
}

void ContextMenuPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

void ContextMenuPopup::setShadowConfig(const ShellConfig::ShadowConfig& shadow) {
  if (m_shadowConfig == shadow) {
    return;
  }
  m_shadowConfig = shadow;
  if (isOpen()) {
    close();
  }
}

bool ContextMenuPopup::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }

  const bool onPopup = (event.surface != nullptr && event.surface == m_wlSurface);

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onPopup) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (onPopup || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      return true;
    }
    break;
  case PointerEvent::Type::Button:
    if (onPopup || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool pressed = (event.state == 1);
      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
      return true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (onPopup || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool consumed = m_inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
      if (m_surface != nullptr && m_sceneRoot != nullptr) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else if (m_sceneRoot->paintDirty() || consumed) {
          m_surface->requestRedraw();
        }
      }
      return consumed || onPopup;
    }
    break;
  }

  if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
    m_surface->requestRedraw();
  }

  return onPopup;
}

void ContextMenuPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }

  const std::uint32_t sym = event.sym;
  const std::uint32_t modifiers = event.modifiers;

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    auto* self = this;
    DeferredCall::callLater([self]() { self->close(); });
    return;
  }

  if (m_menu == nullptr) {
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (m_menu->moveHighlight(1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (m_menu->moveHighlight(-1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    (void)m_menu->activateHighlighted();
  } else if (KeySymbol::isHome(sym)) {
    m_menu->setHighlightedIndex(0);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  } else if (KeySymbol::isEnd(sym)) {
    if (m_menu->entryCount() > 0) {
      m_menu->setHighlightedIndex(m_menu->entryCount() - 1);
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  }
}

void ContextMenuPopup::ensureHighlightedVisible() {
  if (m_menu == nullptr || m_scrollView == nullptr || !m_scrollView->scrollable()) {
    return;
  }
  const std::size_t index = m_menu->highlightedIndex();
  const float rowTop = m_menu->rowTop(index);
  const float rowBottom = m_menu->rowBottom(index);
  const float viewportH = m_scrollView->contentViewportHeight();
  const float offset = m_scrollView->scrollOffset();
  if (rowTop < offset) {
    m_scrollView->setScrollOffset(rowTop);
  } else if (rowBottom > offset + viewportH) {
    m_scrollView->setScrollOffset(rowBottom - viewportH);
  }
}

void ContextMenuPopup::requestVisualUpdate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    m_surface->requestLayout();
  } else {
    m_surface->requestRedraw();
  }
}

void ContextMenuPopup::restoreParentKeyboardInteractivity() {
  if (m_keyboardParentLayerSurface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      m_keyboardParentLayerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
  );
  if (m_keyboardParentWlSurface != nullptr) {
    wl_surface_commit(m_keyboardParentWlSurface);
  }
  m_keyboardParentLayerSurface = nullptr;
  m_keyboardParentWlSurface = nullptr;
}

bool ContextMenuPopup::dispatchKeyboardEvent(const KeyboardEvent& event) {
  if (s_openMenu == nullptr || !s_openMenu->isOpen()) {
    return false;
  }
  s_openMenu->onKeyboardEvent(event);
  return true;
}

wl_surface* ContextMenuPopup::wlSurface() const noexcept { return m_wlSurface; }
