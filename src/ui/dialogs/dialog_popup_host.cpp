#include "ui/dialogs/dialog_popup_host.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/key_symbols.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/builders.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>
#include <vector>

namespace {

  constexpr std::uint32_t kPopupConstraintAdjust = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
      | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
      | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
      | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;

  ShellConfig::ShadowConfig popupShadowConfig(ConfigService* config) {
    return config != nullptr ? config->config().shell.shadow : ShellConfig::ShadowConfig{};
  }

  // Every live DialogPopupHost, used to resolve parent/child popup relationships
  // across instances that don't own each other (e.g. the editor sheet popup and
  // the app-global glyph/color/file pickers parented to it).
  std::vector<DialogPopupHost*>& activeDialogHosts() {
    static std::vector<DialogPopupHost*> hosts;
    return hosts;
  }

} // namespace

DialogPopupHost::DialogPopupHost() { activeDialogHosts().push_back(this); }

DialogPopupHost::~DialogPopupHost() {
  auto& hosts = activeDialogHosts();
  hosts.erase(std::remove(hosts.begin(), hosts.end(), this), hosts.end());
  // Subclass destructors are responsible for calling destroyPopup() before
  // their own members go away — by the time we reach this destructor the
  // subclass vtable has already been replaced and any virtual hook would
  // dispatch to the base no-op, not the subclass override.
  assert(m_surface == nullptr && "subclass must call destroyPopup() in its destructor");
}

void DialogPopupHost::closeChildPopups() {
  wl_surface* const self = wlSurface();
  if (self == nullptr) {
    return;
  }
  // Snapshot: cancelling a child mutates the registry (and may recurse into
  // grandchildren) while we iterate.
  const std::vector<DialogPopupHost*> hosts = activeDialogHosts();
  for (DialogPopupHost* host : hosts) {
    if (host != this && host->isOpen() && host->m_parentSurface == self) {
      host->cancel();
    }
  }
}

void DialogPopupHost::initializeBase(WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext) {
  m_wayland = &wayland;
  m_config = &config;
  m_renderContext = &renderContext;
  m_popupHosts = nullptr;
}

void DialogPopupHost::initializeBase(
    WaylandConnection& wayland, ConfigService& config, RenderContext& renderContext, LayerPopupHostRegistry& popupHosts
) {
  m_wayland = &wayland;
  m_config = &config;
  m_renderContext = &renderContext;
  m_popupHosts = &popupHosts;
}

bool DialogPopupHost::openPopup(std::uint32_t width, std::uint32_t height) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_popupHosts == nullptr) {
    return false;
  }

  destroyPopup();

  const auto parentContext = resolveParentContext();
  if (!parentContext.has_value()) {
    return false;
  }
  m_parentSurface = parentContext->surface;

  auto surface = std::make_unique<PopupSurface>(*m_wayland);
  surface->setRenderContext(m_renderContext);
  surface->setAnimationManager(&m_animations);
  surface->setConfigureCallback([this](std::uint32_t /*w*/, std::uint32_t /*h*/) { requestLayout(); });
  surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  // Defer: the compositor can send popup_done synchronously inside the init roundtrip below (e.g. a
  // grab serial it rejects). Running cancel() there would destroy this popup mid-initialization.
  surface->setDismissedCallback([this]() { DeferredCall::callLater([this]() { cancel(); }); });

  m_chrome =
      popup_chrome::computeGeometry(static_cast<float>(width), static_cast<float>(height), popupShadowConfig(m_config));
  PopupSurfaceConfig popupConfig = defaultPopupConfig(*parentContext, width, height);
  popup_chrome::applyToConfig(
      popupConfig, m_chrome,
      popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Center
      }
  );

  m_surface = std::move(surface);
  m_popupHosts->beginAttachedPopup(m_parentSurface);
  m_attachedToHost = true;
  m_openInProgress = true;
  const bool initialized = parentContext->xdgSurface != nullptr
      ? m_surface->initializeAsChild(parentContext->xdgSurface, parentContext->output, popupConfig)
      : m_surface->initialize(parentContext->layerSurface, parentContext->output, popupConfig);
  m_openInProgress = false;
  if (!initialized) {
    destroyPopup();
    return false;
  }
  if (m_closeRequestedDuringOpen) {
    destroyPopup();
    return false;
  }
  if (m_surface == nullptr) {
    return false;
  }
  popup_chrome::setContentInputRegion(*m_surface, m_chrome);
  return true;
}

bool DialogPopupHost::openPopupAsChild(
    PopupSurfaceConfig config, xdg_surface* parentXdgSurface, wl_surface* parentWlSurface, wl_output* output
) {
  if (m_wayland == nullptr || m_renderContext == nullptr || parentXdgSurface == nullptr || parentWlSurface == nullptr) {
    return false;
  }

  destroyPopup();
  m_parentSurface = parentWlSurface;

  auto surface = std::make_unique<PopupSurface>(*m_wayland);
  surface->setRenderContext(m_renderContext);
  surface->setAnimationManager(&m_animations);
  surface->setConfigureCallback([this](std::uint32_t /*w*/, std::uint32_t /*h*/) { requestLayout(); });
  surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
    prepareFrame(needsUpdate, needsLayout);
  });
  // Defer: the compositor can send popup_done synchronously inside the init roundtrip below (e.g. a
  // grab serial it rejects). Running cancel() there would destroy this popup mid-initialization.
  surface->setDismissedCallback([this]() { DeferredCall::callLater([this]() { cancel(); }); });

  m_chrome = popup_chrome::computeGeometry(
      static_cast<float>(config.width), static_cast<float>(config.height), popupShadowConfig(m_config)
  );
  popup_chrome::applyToConfig(
      config, m_chrome,
      popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Center
      }
  );

  m_surface = std::move(surface);
  m_openInProgress = true;
  const bool initialized = m_surface->initializeAsChild(parentXdgSurface, output, config);
  m_openInProgress = false;
  if (!initialized) {
    destroyPopup();
    return false;
  }
  if (m_closeRequestedDuringOpen) {
    destroyPopup();
    return false;
  }
  if (m_surface == nullptr) {
    return false;
  }
  popup_chrome::setContentInputRegion(*m_surface, m_chrome);
  return true;
}

void DialogPopupHost::destroyPopup() {
  if (m_openInProgress) {
    m_closeRequestedDuringOpen = true;
    return;
  }
  // Topmost-first teardown: any picker popup parented to this one must go before
  // this surface is destroyed, or the compositor raises xdg_popup protocol error
  // 2 ("destroyed while it was not the topmost popup").
  closeChildPopups();
  m_closeRequestedDuringOpen = false;
  if (m_attachedToHost && m_popupHosts != nullptr) {
    m_popupHosts->endAttachedPopup(m_parentSurface);
    m_attachedToHost = false;
  }
  m_pointerInside = false;
  m_parentSurface = nullptr;
  m_inputDispatcher.setTextInputContext(nullptr, nullptr);
  m_inputDispatcher.setSceneRoot(nullptr);
  // onSheetClose hook fires before scene tear-down so subclasses can run
  // any sheet-specific cleanup (e.g. FileDialogView::onClose()) while their
  // sheet pointer is still wired into the scene.
  if (m_sceneRoot != nullptr || m_surface != nullptr) {
    onSheetClose();
  }
  m_bgNode = nullptr;
  m_panelShadow = nullptr;
  m_contentNode = nullptr;
  m_sceneRoot.reset();
  m_surface.reset();
  m_chrome = {};
}

void DialogPopupHost::closeAfterAccept() { destroyPopup(); }

float DialogPopupHost::uiScale() const {
  if (m_config == nullptr) {
    return 1.0f;
  }
  return std::max(0.1f, m_config->config().shell.uiScale);
}

PopupSurfaceConfig DialogPopupHost::defaultPopupConfig(
    const LayerPopupParentContext& parent, std::uint32_t width, std::uint32_t height
) const {
  const auto [offsetX, offsetY] = parent.centeringOffset(*m_wayland);
  return PopupSurfaceConfig{
      .anchorX = 0,
      .anchorY = 0,
      .anchorWidth = static_cast<std::int32_t>(parent.width),
      .anchorHeight = static_cast<std::int32_t>(parent.height),
      .width = width,
      .height = height,
      .anchor = XDG_POSITIONER_ANCHOR_NONE,
      .gravity = XDG_POSITIONER_GRAVITY_NONE,
      .constraintAdjustment = kPopupConstraintAdjust,
      .offsetX = offsetX,
      .offsetY = offsetY,
      .serial = m_wayland->lastInputSerial(),
      .grab = true,
  };
}

bool DialogPopupHost::onPointerEvent(const PointerEvent& event) {
  if (m_surface == nullptr) {
    return false;
  }

  const bool captured = m_inputDispatcher.pointerCaptured();
  float localX = 0.0f;
  float localY = 0.0f;
  const bool mapped = mapPointerEvent(event, localX, localY);
  if (!mapped) {
    if ((event.type == PointerEvent::Type::Leave && event.surface == m_parentSurface)
        || (event.type == PointerEvent::Type::Motion && event.surface == m_parentSurface && m_pointerInside)) {
      m_pointerInside = false;
      if (!captured) {
        m_inputDispatcher.pointerLeave();
      }
      markDirtyTail();
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_pointerInside = true;
    m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_pointerInside = false;
    if (!captured) {
      m_inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (captured) {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    } else if (!m_pointerInside) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    }
    break;
  case PointerEvent::Type::Button:
    if (captured) {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    } else if (!m_pointerInside) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else {
      m_inputDispatcher.pointerMotion(localX, localY, event.serial);
    }
    m_inputDispatcher.pointerButton(localX, localY, event.button, event.state == 1);
    break;
  case PointerEvent::Type::Axis:
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
    break;
  }

  markDirtyTail();
  return true;
}

void DialogPopupHost::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_surface == nullptr) {
    return;
  }

  if (event.pressed && !event.preedit && KeySymbol::isEscape(event.sym)) {
    cancel();
    return;
  }

  if (preDispatchKeyboard(event)) {
    markDirtyTail();
    return;
  }

  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  markDirtyTail();
}

void DialogPopupHost::requestLayout() {
  if (m_surface != nullptr) {
    m_surface->requestLayout();
  }
}

void DialogPopupHost::requestRedraw() {
  if (m_surface != nullptr) {
    m_surface->requestRedraw();
  }
}

void DialogPopupHost::requestUpdateOnly() {
  if (m_surface != nullptr) {
    m_surface->requestUpdateOnly();
  }
}

wl_surface* DialogPopupHost::wlSurface() const noexcept {
  return m_surface != nullptr ? m_surface->wlSurface() : nullptr;
}

xdg_surface* DialogPopupHost::xdgSurface() const noexcept {
  return m_surface != nullptr ? m_surface->xdgSurface() : nullptr;
}

std::uint32_t DialogPopupHost::width() const noexcept { return m_surface != nullptr ? m_surface->width() : 0; }

std::uint32_t DialogPopupHost::height() const noexcept { return m_surface != nullptr ? m_surface->height() : 0; }

void DialogPopupHost::cancel() {
  if (m_surface == nullptr) {
    return;
  }
  destroyPopup();
  cancelToFacade();
}

void DialogPopupHost::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_surface == nullptr || m_renderContext == nullptr) {
    return;
  }

  const auto width = m_surface->width();
  const auto height = m_surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());

  const bool needsSceneBuild = m_sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->height())) != height;
  if (needsSceneBuild) {
    buildScene(width, height);
  }

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    runUpdatePhase();
  }
  if (needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutScene(static_cast<float>(width), static_cast<float>(height));
  }
  if (needsUpdate && m_sceneRoot != nullptr && m_sceneRoot->layoutDirty()) {
    requestLayout();
  }
}

void DialogPopupHost::buildScene(std::uint32_t width, std::uint32_t height) {
  (void)width;
  (void)height;
  m_sceneRoot = std::make_unique<Node>();
  m_sceneRoot->setAnimationManager(&m_animations);
  m_panelShadow = popup_chrome::addShadow(*m_sceneRoot, m_chrome, popupShadowConfig(m_config), Style::scaledRadiusXl());

  auto bg = ui::box({
      .configure = [this](Box& box) {
        box.setPanelStyle(m_config != nullptr && m_config->config().shell.panel.borders);
      },
  });
  m_bgNode = static_cast<Box*>(m_sceneRoot->addChild(std::move(bg)));

  auto content = std::make_unique<Node>();
  m_contentNode = content.get();
  m_sceneRoot->addChild(std::move(content));

  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  // Popup grabs keyboard focus before the compositor delivers a text_input enter; allow IME activation on
  // keyboard focus alone. Other surfaces keep the strict protocol-enter gate.
  m_inputDispatcher.setTextInputContext(
      m_surface->wlSurface(), m_wayland->textInputService(), /*keyboardFocusActivation=*/true
  );
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  m_surface->setSceneRoot(m_sceneRoot.get());

  populateContent(
      m_contentNode, static_cast<std::uint32_t>(std::round(m_chrome.contentWidth)),
      static_cast<std::uint32_t>(std::round(m_chrome.contentHeight))
  );

  if (auto* focusArea = initialFocusArea(); focusArea != nullptr) {
    m_inputDispatcher.setFocus(focusArea);
  }

  layoutScene(static_cast<float>(width), static_cast<float>(height));
  syncPointerStateFromCurrentPosition();

  DeferredCall::callLater([this]() {
    if (m_surface == nullptr) {
      return;
    }
    if (auto* focusArea = initialFocusArea(); focusArea != nullptr) {
      m_inputDispatcher.setFocus(focusArea);
    }
  });
}

void DialogPopupHost::syncSceneGeometryFromSurface() {
  if (m_sceneRoot == nullptr || m_surface == nullptr) {
    return;
  }
  const std::uint32_t surfaceWidth = m_surface->width();
  const std::uint32_t surfaceHeight = m_surface->height();
  if (surfaceWidth == 0 || surfaceHeight == 0) {
    return;
  }

  const float surfW = static_cast<float>(surfaceWidth);
  const float surfH = static_cast<float>(surfaceHeight);
  if (surfaceWidth != m_chrome.surfaceWidth || surfaceHeight != m_chrome.surfaceHeight) {
    const auto& bleed = m_chrome.bleed;
    m_chrome.contentWidth = std::max(1.0f, surfW - static_cast<float>(bleed.left + bleed.right));
    m_chrome.contentHeight = std::max(1.0f, surfH - static_cast<float>(bleed.up + bleed.down));
    m_chrome.surfaceWidth = surfaceWidth;
    m_chrome.surfaceHeight = surfaceHeight;
    popup_chrome::setContentInputRegion(*m_surface, m_chrome);
  }

  m_sceneRoot->setSize(surfW, surfH);
  const float panelX = m_chrome.contentX();
  const float panelY = m_chrome.contentY();
  const float panelW = m_chrome.contentWidth;
  const float panelH = m_chrome.contentHeight;
  if (m_panelShadow != nullptr) {
    const ShellConfig::ShadowConfig shadow = popupShadowConfig(m_config);
    const auto offset = shadowDirectionOffset(shadow.direction);
    m_panelShadow->setPosition(panelX + static_cast<float>(offset.x), panelY + static_cast<float>(offset.y));
    m_panelShadow->setFrameSize(panelW, panelH);
  }
  if (m_bgNode != nullptr) {
    m_bgNode->setPosition(panelX, panelY);
    m_bgNode->setSize(panelW, panelH);
  }
  const float padding = computePadding(uiScale());
  const float contentWidth = panelW - padding * 2.0f;
  const float contentHeight = panelH - padding * 2.0f;
  if (m_contentNode != nullptr) {
    m_contentNode->setPosition(panelX + padding, panelY + padding);
    m_contentNode->setSize(contentWidth, contentHeight);
  }
}

void DialogPopupHost::layoutScene(float width, float height) {
  (void)width;
  (void)height;
  if (m_sceneRoot == nullptr || m_surface == nullptr) {
    return;
  }

  syncSceneGeometryFromSurface();

  const float padding = computePadding(uiScale());
  float contentWidth = m_chrome.contentWidth - padding * 2.0f;
  float contentHeight = m_chrome.contentHeight - padding * 2.0f;

  layoutSheet(contentWidth, contentHeight);

  syncSceneGeometryFromSurface();
}

bool DialogPopupHost::mapPointerEvent(const PointerEvent& event, float& localX, float& localY) const noexcept {
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
    if (eventSurface == m_parentSurface) {
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

  if (eventSurface != m_parentSurface) {
    return false;
  }

  localX = static_cast<float>(event.sx) - static_cast<float>(m_surface->configuredX());
  localY = static_cast<float>(event.sy) - static_cast<float>(m_surface->configuredY());

  if (event.type == PointerEvent::Type::Leave) {
    return m_pointerInside || m_inputDispatcher.pointerCaptured();
  }

  const float left = m_chrome.contentX();
  const float top = m_chrome.contentY();
  const float right = m_chrome.contentRight();
  const float bottom = m_chrome.contentBottom();
  if (m_inputDispatcher.pointerCaptured()) {
    return true;
  }
  if (event.type == PointerEvent::Type::Button && m_pointerInside) {
    return true;
  }
  return localX >= left && localY >= top && localX < right && localY < bottom;
}

void DialogPopupHost::syncPointerStateFromCurrentPosition() {
  if (m_wayland == nullptr || m_surface == nullptr || !m_wayland->hasPointerPosition()) {
    return;
  }

  PointerEvent synthetic;
  synthetic.type = PointerEvent::Type::Motion;
  synthetic.surface = m_wayland->lastPointerSurface();
  synthetic.sx = m_wayland->lastPointerX();
  synthetic.sy = m_wayland->lastPointerY();
  synthetic.serial = m_wayland->lastInputSerial();

  float localX = 0.0f;
  float localY = 0.0f;
  if (!mapPointerEvent(synthetic, localX, localY)) {
    return;
  }

  m_pointerInside = true;
  m_inputDispatcher.pointerEnter(localX, localY, synthetic.serial);
  markDirtyTail();
}

bool DialogPopupHost::ownsSurface(wl_surface* surface) const noexcept {
  return m_surface != nullptr && surface != nullptr && surface == m_surface->wlSurface();
}

wl_surface* DialogPopupHost::resolveEventSurface(const PointerEvent& event) const noexcept {
  wl_surface* eventSurface = event.surface;
  if (eventSurface == nullptr && event.type == PointerEvent::Type::Motion && m_wayland != nullptr) {
    eventSurface = m_wayland->lastPointerSurface();
  }
  return eventSurface;
}

std::optional<LayerPopupParentContext> DialogPopupHost::resolveParentContext() const {
  if (m_wayland == nullptr || m_popupHosts == nullptr) {
    return std::nullopt;
  }
  return m_popupHosts->resolveForInput(*m_wayland);
}

void DialogPopupHost::markDirtyTail() {
  if (m_sceneRoot == nullptr) {
    return;
  }
  if (!m_sceneRoot->paintDirty() && !m_sceneRoot->layoutDirty()) {
    return;
  }
  if (m_sceneRoot->layoutDirty()) {
    requestLayout();
  } else {
    requestRedraw();
  }
}
