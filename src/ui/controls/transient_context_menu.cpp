#include "ui/controls/transient_context_menu.h"

#include "core/log.h"
#include "render/render_context.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace {
  constexpr Logger kLog("transient-context-menu");
  constexpr auto kPointerDiscoveryTimeout = std::chrono::milliseconds(1500);
} // namespace

TransientContextMenu::~TransientContextMenu() { close(); }

void TransientContextMenu::initialize(WaylandConnection& wayland, RenderContext& renderContext) {
  m_wayland = &wayland;
  m_renderContext = &renderContext;
  m_clickShield.initialize(wayland);
}

bool TransientContextMenu::open(TransientContextMenuRequest request, ActivateCallback onActivate) {
  close();
  if (m_wayland == nullptr || m_renderContext == nullptr || request.entries.empty() || !onActivate) {
    return false;
  }

  std::vector<wl_output*> outputs;
  outputs.reserve(m_wayland->outputs().size());
  for (const auto& output : m_wayland->outputs()) {
    if (output.output != nullptr && output.done) {
      outputs.push_back(output.output);
    }
  }
  if (outputs.empty()) {
    kLog.warn("cannot open: no configured output");
    return false;
  }

  m_pendingRequest = std::make_unique<TransientContextMenuRequest>(std::move(request));
  m_onActivate = std::move(onActivate);
  // Exclusive is deliberate here on every compositor: this is a modal menu,
  // and unlike panel shields the host must also give its child popup keyboard
  // focus without relying on a compositor-specific focus-grab protocol.
  m_clickShield.activate(outputs, LayerShellLayer::Overlay, {}, LayerShellKeyboard::Exclusive);
  if (!m_clickShield.isActive()) {
    kLog.warn("cannot open: layer-shell click shield unavailable");
    m_pendingRequest.reset();
    m_onActivate = {};
    return false;
  }

  m_pointerWaitTimer.start(kPointerDiscoveryTimeout, [this]() {
    if (m_pendingRequest != nullptr) {
      kLog.warn("closing: compositor did not provide cursor position");
      close();
    }
  });
  return true;
}

void TransientContextMenu::close() {
  if (m_closing) {
    return;
  }
  m_closing = true;
  m_pointerWaitTimer.stop();
  m_pendingRequest.reset();
  m_onActivate = {};
  // The xdg_popup must be destroyed before its layer-shell parent surfaces.
  // Keep the ContextMenuPopup object itself alive because its dismissal callback
  // may currently be running on this stack.
  if (m_popup != nullptr) {
    m_popup->setOnDismissed(nullptr);
    m_popup->close();
  }
  m_clickShield.deactivate();
  m_closing = false;
}

bool TransientContextMenu::isActive() const noexcept {
  return m_pendingRequest != nullptr || (m_popup != nullptr && m_popup->isOpen()) || m_clickShield.isActive();
}

bool TransientContextMenu::onPointerEvent(const PointerEvent& event) {
  if (!isActive()) {
    return false;
  }
  if (m_popup != nullptr && m_popup->onPointerEvent(event)) {
    return true;
  }
  if (!m_clickShield.ownsSurface(event.surface)) {
    return false;
  }

  if (event.type == PointerEvent::Type::Enter && m_pendingRequest != nullptr) {
    openPopup(event);
    return true;
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    close();
    return true;
  }
  // The shield is modal while active. Consume its motion/axis/release events so
  // no unrelated shell component interprets coordinates from this surface.
  return true;
}

void TransientContextMenu::openPopup(const PointerEvent& event) {
  if (m_pendingRequest == nullptr || m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }
  const auto parent = m_clickShield.surfaceContext(event.surface);
  if (!parent.has_value() || parent->layerSurface == nullptr || parent->output == nullptr) {
    close();
    return;
  }

  m_pointerWaitTimer.stop();
  auto request = std::move(*m_pendingRequest);
  m_pendingRequest.reset();
  if (m_popup == nullptr) {
    m_popup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
  }

  const ActivateCallback activate = m_onActivate;
  m_popup->setOnActivate([activate](const ContextMenuControlEntry& entry) {
    if (activate) {
      activate(entry);
    }
  });
  m_popup->setOnDismissed([this]() { close(); });

  const auto anchorX = static_cast<std::int32_t>(std::lround(event.sx));
  const auto anchorY = static_cast<std::int32_t>(std::lround(event.sy));
  m_popup->open(
      ContextMenuPopupRequest{
          .entries = std::move(request.entries),
          .maxMenuWidth = request.maxMenuWidth > 0.0F ? request.maxMenuWidth : Style::menuAutoMaxWidth,
          .contentScale = std::max(0.1F, request.contentScale),
          .maxVisible = request.maxVisible,
          .anchor = PopupAnchorRect{.x = anchorX, .y = anchorY, .width = 1, .height = 1},
          .parent =
              PopupSurfaceParent{
                  .layerSurface = parent->layerSurface,
                  .output = parent->output,
              },
          .pointerParentSurface = parent->surface,
          // IPC has no originating Wayland input serial. Zero intentionally
          // disables xdg_popup_grab; the layer-shell host is the portable grab.
          .inputSerial = 0,
      }
  );
  if (!m_popup->isOpen()) {
    close();
  }
}
