#include "shell/tray/tray_menu.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/tray/tray_service.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/panel/panel_manager.h"
#include "shell/tray/tray_identifier.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace {

  constexpr Logger kLog("tray");

  constexpr float kMenuWidth = 246.0f;
  constexpr std::size_t kTrayMenuVisibleItems = 20;
  constexpr std::int32_t kPinToggleEntryId = -2147000000;

  constexpr float kSurfaceWidth = kMenuWidth;

  constexpr std::uint32_t kPopupConstraintAdjust =
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;

  bool containsTrayWidget(const std::vector<std::string>& widgets) {
    return std::find(widgets.begin(), widgets.end(), "tray") != widgets.end();
  }

  void closeTrayDrawerPanelIfOpen() {
    auto& panelManager = PanelManager::instance();
    if (panelManager.isOpenPanel("tray-drawer")) {
      panelManager.close();
    }
  }

  bool trayDrawerEnabled(ConfigService* config) {
    if (config == nullptr) {
      return false;
    }
    const auto it = config->config().widgets.find("tray");
    if (it == config->config().widgets.end()) {
      return false;
    }
    return it->second.getBool("drawer", false);
  }

  std::size_t visibleEntryLimit(std::size_t entryCount) {
    return std::max<std::size_t>(1, std::min<std::size_t>(entryCount, kTrayMenuVisibleItems));
  }

  // Convert an icon name like "audio-input-microphone-symbolic" to a readable label like "Audio Input Microphone".
  std::string iconNameToLabel(std::string_view iconName) {
    // Strip trailing "-symbolic"
    constexpr std::string_view kSymbolicSuffix = "-symbolic";
    if (iconName.ends_with(kSymbolicSuffix)) {
      iconName.remove_suffix(kSymbolicSuffix.size());
    }
    std::string out;
    out.reserve(iconName.size());
    bool capitaliseNext = true;
    for (char c : iconName) {
      if (c == '-') {
        out.push_back(' ');
        capitaliseNext = true;
      } else if (capitaliseNext) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        capitaliseNext = false;
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  std::optional<BarConfig> resolveTrayBarConfig(ConfigService* config, WaylandConnection* wayland, wl_output* output) {
    if (config == nullptr) {
      return std::nullopt;
    }

    const WaylandOutput* wlOutput = nullptr;
    if (wayland != nullptr && output != nullptr) {
      wlOutput = wayland->findOutputByWl(output);
    }

    std::optional<BarConfig> fallback;
    for (const auto& base : config->config().bars) {
      BarConfig resolved = base;
      if (wlOutput != nullptr) {
        resolved = ConfigService::resolveForOutput(base, *wlOutput);
      }
      if (!resolved.enabled) {
        continue;
      }
      if (!fallback.has_value()) {
        fallback = resolved;
      }
      if (containsTrayWidget(resolved.startWidgets) || containsTrayWidget(resolved.centerWidgets) ||
          containsTrayWidget(resolved.endWidgets)) {
        return resolved;
      }
    }
    return fallback;
  }

  struct PopupPlacement {
    std::int32_t anchorX = 0;
    std::int32_t anchorY = 0;
    std::int32_t anchorWidth = 1;
    std::int32_t anchorHeight = 1;
    std::uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
    std::uint32_t gravity = XDG_POSITIONER_GRAVITY_TOP;
    std::int32_t offsetX = 0;
    std::int32_t offsetY = 2;
    popup_chrome::Attachment chromeAttachment{
        .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
    };
    ContextSubmenuDirection submenuDirection = ContextSubmenuDirection::Right;
  };

  PopupPlacement popupPlacementForBar(const BarConfig& bar, std::int32_t anchorX, std::int32_t anchorY) {
    const std::int32_t kGap = std::max(2, static_cast<std::int32_t>(Style::spaceMd));
    const std::int32_t iconSize = std::clamp(bar.thickness - 10, 16, 40);
    const std::int32_t halfIcon = iconSize / 2;
    PopupPlacement placement{
        .anchorX = anchorX - halfIcon,
        .anchorY = anchorY - halfIcon,
        .anchorWidth = iconSize,
        .anchorHeight = iconSize,
    };

    if (bar.position == "bottom") {
      placement.anchor = XDG_POSITIONER_ANCHOR_TOP;
      placement.gravity = XDG_POSITIONER_GRAVITY_TOP;
      placement.offsetX = 0;
      placement.offsetY = -kGap;
      placement.chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center,
          .vertical = popup_chrome::VerticalAttachment::Bottom,
      };
      placement.submenuDirection = ContextSubmenuDirection::Right;
      return placement;
    }

    if (bar.position == "left") {
      placement.anchor = XDG_POSITIONER_ANCHOR_RIGHT;
      placement.gravity = XDG_POSITIONER_GRAVITY_RIGHT;
      placement.offsetX = kGap;
      placement.offsetY = 0;
      placement.chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Left,
          .vertical = popup_chrome::VerticalAttachment::Center,
      };
      placement.submenuDirection = ContextSubmenuDirection::Right;
      return placement;
    }

    if (bar.position == "right") {
      placement.anchor = XDG_POSITIONER_ANCHOR_LEFT;
      placement.gravity = XDG_POSITIONER_GRAVITY_LEFT;
      placement.offsetX = -kGap;
      placement.offsetY = 0;
      placement.chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Right,
          .vertical = popup_chrome::VerticalAttachment::Center,
      };
      placement.submenuDirection = ContextSubmenuDirection::Left;
      return placement;
    }

    placement.anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
    placement.gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
    placement.offsetX = 0;
    placement.offsetY = kGap;
    placement.chromeAttachment = popup_chrome::Attachment{
        .horizontal = popup_chrome::HorizontalAttachment::Center,
        .vertical = popup_chrome::VerticalAttachment::Top,
    };
    placement.submenuDirection = ContextSubmenuDirection::Right;
    return placement;
  }

  ShellConfig::ShadowConfig popupShadowConfig(ConfigService* config) {
    return config != nullptr ? config->config().shell.shadow : ShellConfig::ShadowConfig{};
  }

} // namespace

void TrayMenu::initialize(
    WaylandConnection& wayland, ConfigService* config, TrayService* tray, RenderContext* renderContext
) {
  m_wayland = &wayland;
  m_config = config;
  m_tray = tray;
  m_renderContext = renderContext;
}

void TrayMenu::onTrayChanged() {
  if (!m_visible) {
    return;
  }

  auto previousEntries = std::move(m_entries);
  refreshEntries();
  if (m_entries.empty()) {
    close();
    return;
  }
  if (m_entries == previousEntries) {
    return;
  }

  resizeMainSurfaceToEntries();
  rebuildScenes();

  if (m_pendingSubmenuParentEntryId != 0 && m_submenuInstance == nullptr) {
    const auto parentId = m_pendingSubmenuParentEntryId;
    const auto rowCenterY = m_pendingSubmenuRowCenterY;
    m_pendingSubmenuParentEntryId = 0;
    m_pendingSubmenuRowCenterY = 0.0f;
    openSubmenu(parentId, rowCenterY);
  }
}

void TrayMenu::toggleForItem(const std::string& itemId) {
  if (itemId.empty()) {
    close();
    return;
  }

  if (m_visible && itemId == m_activeItemId) {
    close();
    return;
  }

  // popup_done fires on press; setOnClick fires on release. By the time we get here
  // the menu is already closed (m_visible = false) even though the user is closing it.
  // Suppress reopening if the same item was dismissed within the last 300 ms.
  if (!m_visible && itemId == m_lastClosedItemId) {
    const auto elapsed = std::chrono::steady_clock::now() - m_lastCloseTime;
    if (elapsed < std::chrono::milliseconds(300)) {
      m_lastClosedItemId.clear();
      return;
    }
  }

  m_activeItemId = itemId;

  // Some dbusmenu servers only materialize menu rows after receiving "opened".
  // Emit this before the first fetch so we don't render a persistent empty menu.
  if (m_tray != nullptr) {
    m_tray->notifyMenuOpened(m_activeItemId);
  }

  refreshEntries();

  m_visible = true;
  ensureSurface();
  if (m_instance == nullptr || m_instance->surface == nullptr) {
    close();
    return;
  }

  rebuildScenes();
}

void TrayMenu::close() {
  if (!m_visible) {
    return;
  }
  m_lastClosedItemId = m_activeItemId;
  m_lastCloseTime = std::chrono::steady_clock::now();
  m_visible = false;
  // Stop any in-flight retry: continuing to hit GetLayout while the user is
  // spam-clicking the tray is what wedges Electron's dbusmenu handler.
  m_retryTimer.stop();
  closeSubmenu();
  // Send the "closed" event before tearing down the surface so the server
  // has a chance to reset its internal open-state before the next open.
  if (m_tray != nullptr && !m_activeItemId.empty()) {
    m_tray->notifyMenuClosed(m_activeItemId);
  }
  destroySurface();
}

void TrayMenu::onThemeChanged() {
  if (!m_visible) {
    return;
  }
  rebuildScenes();
}

void TrayMenu::requestLayout() {
  if (!m_visible) {
    return;
  }
  if (m_instance != nullptr && m_instance->surface != nullptr) {
    m_instance->surface->requestLayout();
  }
  if (m_submenuInstance != nullptr && m_submenuInstance->surface != nullptr) {
    m_submenuInstance->surface->requestLayout();
  }
}

bool TrayMenu::onPointerEvent(const PointerEvent& event) {
  if (!m_visible || m_instance == nullptr) {
    return false;
  }

  // Route to submenu first — it holds the active grab when open.
  if (m_submenuInstance != nullptr) {
    auto* sub = m_submenuInstance.get();
    const bool onSub = (event.surface != nullptr && event.surface == sub->wlSurface);
    bool subConsumed = false;

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onSub) {
        sub->pointerInside = true;
        sub->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (onSub) {
        sub->pointerInside = false;
        sub->inputDispatcher.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (onSub || sub->pointerInside) {
        if (onSub)
          sub->pointerInside = true;
        sub->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        subConsumed = true;
      }
      break;
    case PointerEvent::Type::Button:
      if (onSub || sub->pointerInside) {
        if (onSub)
          sub->pointerInside = true;
        const bool pressed = (event.state == 1);
        sub->inputDispatcher.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
        );
        subConsumed = true;
        if (m_submenuInstance == nullptr) {
          return subConsumed;
        }
      }
      break;
    case PointerEvent::Type::Axis:
      if (onSub || sub->pointerInside) {
        if (onSub)
          sub->pointerInside = true;
        subConsumed = sub->inputDispatcher.pointerAxis(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
            event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      break;
    }

    if (sub->surface != nullptr && sub->sceneRoot != nullptr &&
        (sub->sceneRoot->paintDirty() || sub->sceneRoot->layoutDirty())) {
      if (sub->sceneRoot->layoutDirty()) {
        sub->surface->requestLayout();
      } else {
        sub->surface->requestRedraw();
      }
    }

    if (subConsumed) {
      return subConsumed;
    }
  }

  auto* inst = m_instance.get();
  const bool onThisSurface = (event.surface != nullptr && event.surface == inst->wlSurface);
  bool consumed = false;

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onThisSurface) {
      inst->pointerInside = true;
      inst->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  case PointerEvent::Type::Leave:
    if (onThisSurface) {
      inst->pointerInside = false;
      inst->inputDispatcher.pointerLeave();
    }
    break;
  case PointerEvent::Type::Motion:
    if (onThisSurface || inst->pointerInside) {
      if (onThisSurface) {
        inst->pointerInside = true;
      }
      inst->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      consumed = true;
    }
    break;
  case PointerEvent::Type::Button:
    if (onThisSurface || inst->pointerInside) {
      if (onThisSurface) {
        inst->pointerInside = true;
      }
      const bool pressed = (event.state == 1);
      inst->inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
      );
      consumed = true;
      if (!m_visible || m_instance == nullptr) {
        return consumed;
      }
    }
    break;
  case PointerEvent::Type::Axis:
    if (onThisSurface || inst->pointerInside) {
      if (onThisSurface) {
        inst->pointerInside = true;
      }
      consumed = inst->inputDispatcher.pointerAxis(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
          event.axisDiscrete, event.axisValue120, event.axisLines
      );
    }
    break;
  }

  if (inst->surface != nullptr && inst->sceneRoot != nullptr &&
      (inst->sceneRoot->paintDirty() || inst->sceneRoot->layoutDirty())) {
    if (inst->sceneRoot->layoutDirty()) {
      inst->surface->requestLayout();
    } else {
      inst->surface->requestRedraw();
    }
  }

  if (event.type == PointerEvent::Type::Button && event.state == 1 && !consumed) {
    close();
  }
  return consumed;
}

void TrayMenu::onFontChanged() {
  if (m_instance != nullptr && m_instance->surface != nullptr) {
    m_instance->surface->requestLayout();
  }
  if (m_submenuInstance != nullptr && m_submenuInstance->surface != nullptr) {
    m_submenuInstance->surface->requestLayout();
  }
}

void TrayMenu::refreshEntries() {
  m_retryTimer.stop();
  m_entries.clear();
  if (m_tray == nullptr || m_activeItemId.empty()) {
    return;
  }
  m_entries = m_tray->menuEntries(m_activeItemId);
  if (!m_entries.empty() && trayDrawerEnabled(m_config)) {
    const bool pinned = activeItemPinned();
    m_entries.insert(
        m_entries.begin(), TrayMenuEntry{
                               .id = kPinToggleEntryId,
                               .label = i18n::tr(pinned ? "tray.menu.unpin" : "tray.menu.pin"),
                               .iconName = {},
                               .iconData = {},
                               .enabled = true,
                               .visible = true,
                               .separator = false,
                               .hasSubmenu = false,
                           }
    );
  }
  if (m_entries.empty()) {
    m_entries.push_back(
        TrayMenuEntry{
            .id = -1,
            .label = i18n::tr("tray.menu.empty"),
            .iconName = {},
            .iconData = {},
            .enabled = false,
            .visible = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
    // Short retry window for apps that need a moment to populate after registration.
    // LayoutUpdated from TrayService will also trigger a refresh via onTrayChanged,
    // so this is just a fallback for servers that don't emit it reliably.
    scheduleEntryRetry(0);
  }
}

void TrayMenu::scheduleEntryRetry(int attempt) {
  // Delays: 300ms, 900ms, 2000ms — total window ~3s. Kept small on purpose:
  // longer retry loops hammer the server while the user is clicking and that is
  // what wedges Electron's dbusmenu handler.
  constexpr int kDelays[] = {300, 900, 2000};
  constexpr int kMaxAttempts = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
  if (attempt >= kMaxAttempts || m_tray == nullptr) {
    return;
  }
  const auto delay = std::chrono::milliseconds(kDelays[attempt]);
  const std::string capturedItemId = m_activeItemId;
  m_retryTimer.start(delay, [this, attempt, capturedItemId]() {
    // Abort if the menu closed or the user switched tray items — we only retry
    // while the placeholder menu is still visible to the user.
    if (!m_visible || m_tray == nullptr || m_activeItemId != capturedItemId) {
      return;
    }
    auto fresh = m_tray->menuEntries(capturedItemId);
    if (fresh.empty()) {
      scheduleEntryRetry(attempt + 1);
      return;
    }
    kLog.debug("tray menu recovered (attempt {}) for id={}", attempt + 1, capturedItemId);
    m_entries = std::move(fresh);
    if (!m_entries.empty() && trayDrawerEnabled(m_config)) {
      const bool pinned = activeItemPinned();
      m_entries.insert(
          m_entries.begin(), TrayMenuEntry{
                                 .id = kPinToggleEntryId,
                                 .label = i18n::tr(pinned ? "tray.menu.unpin" : "tray.menu.pin"),
                                 .iconName = {},
                                 .iconData = {},
                                 .enabled = true,
                                 .visible = true,
                                 .separator = false,
                                 .hasSubmenu = false,
                             }
      );
    }
    resizeMainSurfaceToEntries();
    rebuildScenes();
  });
}

uint32_t TrayMenu::submenuHeightPx() const {
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_submenuEntries.size());
  for (const auto& entry : m_submenuEntries) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = entry.id,
            .label = entry.label,
            .enabled = entry.enabled,
            .separator = entry.separator,
            .hasSubmenu = entry.hasSubmenu,
            .checkmark = entry.checkmark,
            .radio = entry.radio,
            .toggleState = entry.toggleState,
        }
    );
  }
  return static_cast<uint32_t>(ContextMenuControl::preferredHeight(entries, visibleEntryLimit(entries.size())));
}

uint32_t TrayMenu::surfaceHeightPx() const {
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = entry.id,
            .label = entry.label.empty() ? iconNameToLabel(entry.iconName) : entry.label,
            .enabled = entry.enabled,
            .separator = entry.separator,
            .hasSubmenu = entry.hasSubmenu,
            .checkmark = entry.checkmark,
            .radio = entry.radio,
            .toggleState = entry.toggleState,
        }
    );
  }
  return static_cast<uint32_t>(ContextMenuControl::preferredHeight(entries, visibleEntryLimit(entries.size())));
}

bool TrayMenu::ownsSurface(wl_surface* surface) const {
  return m_instance != nullptr && surface != nullptr && m_instance->wlSurface == surface;
}

void TrayMenu::ensureSurface() {
  if (m_instance != nullptr) {
    return;
  }
  if (m_wayland == nullptr || m_renderContext == nullptr) {
    return;
  }

  wl_surface* parentWlSurface = m_wayland->lastPointerSurface();
  auto* parentLayerSurface = m_wayland->layerSurfaceFor(parentWlSurface);
  wl_output* output = m_wayland->lastPointerOutput();
  const std::uint32_t serial = m_wayland->lastInputSerial();

  if (parentLayerSurface == nullptr || output == nullptr || serial == 0) {
    kLog.debug(
        "tray menu: missing popup anchor context (parent={}, output={}, serial={})", parentLayerSurface != nullptr,
        output != nullptr, serial
    );
    return;
  }

  int anchorX = 0;
  int anchorY = 0;
  if (m_wayland->hasPointerPosition()) {
    anchorX = static_cast<int>(m_wayland->lastPointerX());
    anchorY = static_cast<int>(m_wayland->lastPointerY());
  }

  auto inst = std::make_unique<MenuInstance>();
  inst->output = output;
  inst->surface = std::make_unique<PopupSurface>(*m_wayland);
  inst->surface->setRenderContext(m_renderContext);
  auto* instPtr = inst.get();

  inst->surface->setConfigureCallback([instPtr](uint32_t /*width*/, uint32_t /*height*/) {
    instPtr->surface->requestLayout();
  });
  inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
    prepareMainMenuFrame(*instPtr, needsUpdate, needsLayout);
  });
  inst->surface->setDismissedCallback([this]() { close(); });

  const auto chrome =
      popup_chrome::computeGeometry(kSurfaceWidth, static_cast<float>(surfaceHeightPx()), popupShadowConfig(m_config));
  PopupPlacement placement{};
  if (const auto bar = resolveTrayBarConfig(m_config, m_wayland, output); bar.has_value()) {
    placement = popupPlacementForBar(*bar, anchorX, anchorY);
    anchorX = placement.anchorX;
    anchorY = placement.anchorY;
  }
  // On Hyprland, hyprland_focus_grab_v1 conflicts with xdg_popup_grab — the
  // compositor sends popup_done shortly after we commit our focus grab,
  // tearing the popup down. Skip xdg_popup_grab when we'll be using
  // focus_grab; outside-click dismissal is handled by the focus grab's
  // `cleared` event instead.
  auto* grabService = m_wayland->focusGrabService();
  const bool useFocusGrab = grabService != nullptr && grabService->available();
  inst->submenuDirection = placement.submenuDirection;
  inst->chrome = chrome;
  auto popupConfig = PopupSurfaceConfig{
      .anchorX = anchorX,
      .anchorY = anchorY,
      .anchorWidth = placement.anchorWidth,
      .anchorHeight = placement.anchorHeight,
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = placement.anchor,
      .gravity = placement.gravity,
      .constraintAdjustment = kPopupConstraintAdjust,
      .offsetX = placement.offsetX,
      .offsetY = placement.offsetY,
      .serial = serial,
      .grab = !useFocusGrab,
  };
  popup_chrome::applyToConfig(popupConfig, chrome, placement.chromeAttachment);

  if (!inst->surface->initialize(parentLayerSurface, output, popupConfig)) {
    kLog.debug("tray menu: failed to create popup surface");
    return;
  }

  popup_chrome::setContentInputRegion(*inst->surface, inst->chrome);
  inst->wlSurface = inst->surface->wlSurface();
  m_instance = std::move(inst);

  // Hyprland: without an active focus grab covering the popup, the compositor
  // only delivers pointer events after a click transfers focus from the bar's
  // OnDemand layer surface, so hover never reaches the popup. Whitelisting the
  // popup (and the parent bar so re-clicking other tray icons keeps working)
  // eagerly routes motion to the popup. Defer to the next tick — Hyprland's
  // focus_grab needs the whitelisted surfaces to be mapped, which only happens
  // after the configure round-trip completes; committing the grab synchronously
  // here makes the compositor fire `cleared` immediately and the menu never
  // appears.
  DeferredCall::callLater([this]() {
    if (!m_visible || m_instance == nullptr || m_wayland == nullptr) {
      return;
    }
    auto* svc = m_wayland->focusGrabService();
    if (svc == nullptr || !svc->available()) {
      return;
    }
    m_focusGrab = svc->createGrab();
    if (m_focusGrab == nullptr) {
      return;
    }
    m_focusGrab->setOnCleared([this]() {
      if (m_visible) {
        close();
      }
    });
    m_focusGrab->addSurface(m_instance->wlSurface);
    m_focusGrab->commit();
  });
}

void TrayMenu::resizeMainSurfaceToEntries() {
  if (m_instance == nullptr || m_instance->surface == nullptr) {
    return;
  }

  const auto chrome =
      popup_chrome::computeGeometry(kSurfaceWidth, static_cast<float>(surfaceHeightPx()), popupShadowConfig(m_config));
  const auto desiredWidth = chrome.surfaceWidth;
  const auto desiredHeight = chrome.surfaceHeight;
  if (m_instance->surface->width() == desiredWidth && m_instance->surface->height() == desiredHeight) {
    return;
  }

  closeSubmenu();
  m_instance->chrome = chrome;
  popup_chrome::setContentInputRegion(*m_instance->surface, m_instance->chrome);
  if (!m_instance->surface->resize(desiredWidth, desiredHeight)) {
    m_instance->surface->requestLayout();
  }
}

void TrayMenu::destroySurface() {
  if (m_instance != nullptr) {
    m_instance->inputDispatcher.setSceneRoot(nullptr);
  }
  m_instance.reset();
  m_focusGrab.reset();
}

void TrayMenu::rebuildScenes() {
  uiAssertNotRendering("TrayMenu::rebuildScenes");
  if (!m_visible) {
    return;
  }
  if (!m_entries.empty() && m_instance != nullptr && m_instance->surface != nullptr) {
    m_instance->surface->requestLayout();
  }
  if (!m_submenuEntries.empty() && m_submenuInstance != nullptr && m_submenuInstance->surface != nullptr) {
    m_submenuInstance->surface->requestLayout();
  }
}

void TrayMenu::prepareMainMenuFrame(MenuInstance& inst, bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->width())) != width ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->height())) != height;
  if (needsSceneBuild || needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildScene(inst, width, height);
  }
}

void TrayMenu::buildScene(MenuInstance& inst, uint32_t width, uint32_t height) {
  uiAssertNotRendering("TrayMenu::buildScene");
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  inst.sceneRoot = std::make_unique<Node>();
  inst.sceneRoot->setSize(w, h);
  (void)popup_chrome::addShadow(*inst.sceneRoot, inst.chrome, popupShadowConfig(m_config), Style::scaledRadiusLg());

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = entry.id,
            .label = entry.label.empty() ? iconNameToLabel(entry.iconName) : entry.label,
            .enabled = entry.enabled,
            .separator = entry.separator,
            .hasSubmenu = entry.hasSubmenu,
            .checkmark = entry.checkmark,
            .radio = entry.radio,
            .toggleState = entry.toggleState,
        }
    );
  }

  const float menuWidth = std::max(1.0f, inst.chrome.contentWidth);

  auto scrollView = std::make_unique<ScrollView>();
  scrollView->setPosition(inst.chrome.contentX(), inst.chrome.contentY());
  scrollView->setSize(inst.chrome.contentWidth, inst.chrome.contentHeight);
  scrollView->setViewportPaddingH(0.0f);
  scrollView->setViewportPaddingV(0.0f);
  scrollView->clearFill();
  scrollView->clearBorder();
  scrollView->setRadius(0.0f);
  scrollView->bindState(&inst.scrollState);
  scrollView->setScrollbarVisible(true);

  auto menu = std::make_unique<ContextMenuControl>();
  menu->setMenuWidth(menuWidth);
  menu->setMaxVisible(entries.size()); // Always lay out all entries for scrolling
  menu->setSubmenuDirection(inst.submenuDirection);
  menu->setEntries(std::move(entries));
  menu->setRedrawCallback([&inst]() {
    if (inst.surface != nullptr) {
      inst.surface->requestRedraw();
    }
  });
  menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
    if (entry.id == kPinToggleEntryId) {
      DeferredCall::callLater([this]() {
        (void)toggleActiveItemPinned();
        close();
        closeTrayDrawerPanelIfOpen();
      });
      return;
    }
    if (m_tray == nullptr || m_activeItemId.empty()) {
      return;
    }
    DeferredCall::callLater([this, entry]() {
      if (m_tray != nullptr) {
        (void)m_tray->activateMenuEntry(m_activeItemId, entry.id);
      }
      close();
      closeTrayDrawerPanelIfOpen();
    });
  });
  menu->setOnSubmenuOpen([this](const ContextMenuControlEntry& entry, float rowCenterY) {
    openSubmenu(entry.id, rowCenterY);
  });
  scrollView->content()->addChild(std::move(menu));
  scrollView->layout(*m_renderContext);
  inst.sceneRoot->addChild(std::move(scrollView));

  inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
  inst.inputDispatcher.setCursorShapeCallback([this](uint32_t serial, uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  inst.surface->setSceneRoot(inst.sceneRoot.get());
}

std::optional<TrayItemInfo> TrayMenu::activeTrayItem() const {
  if (m_tray == nullptr || m_activeItemId.empty()) {
    return std::nullopt;
  }
  const auto allItems = m_tray->items();
  const auto it =
      std::ranges::find_if(allItems, [this](const TrayItemInfo& item) { return item.id == m_activeItemId; });
  if (it == allItems.end()) {
    return std::nullopt;
  }
  return *it;
}

bool TrayMenu::activeItemPinned() const {
  if (m_config == nullptr) {
    return false;
  }
  const auto item = activeTrayItem();
  if (!item.has_value()) {
    return false;
  }
  const auto cfgIt = m_config->config().widgets.find("tray");
  const auto pinned =
      cfgIt != m_config->config().widgets.end() ? cfgIt->second.getStringList("pinned") : std::vector<std::string>{};
  for (const auto& token : pinned) {
    if (tray::tokenMatchesItem(token, *item)) {
      return true;
    }
  }
  return false;
}

bool TrayMenu::toggleActiveItemPinned() {
  if (m_config == nullptr) {
    return false;
  }
  const auto item = activeTrayItem();
  if (!item.has_value()) {
    return false;
  }

  auto cfgIt = m_config->config().widgets.find("tray");
  std::vector<std::string> pinned =
      cfgIt != m_config->config().widgets.end() ? cfgIt->second.getStringList("pinned") : std::vector<std::string>{};
  std::erase_if(pinned, [](const std::string& token) {
    return tray::looksGenericStatusItemName(token) || tray::isTransientUniqueIdentifier(token);
  });
  const bool currentlyPinned =
      std::ranges::any_of(pinned, [&](const std::string& token) { return tray::tokenMatchesItem(token, *item); });

  if (currentlyPinned) {
    std::erase_if(pinned, [&](const std::string& token) { return tray::tokenMatchesItem(token, *item); });
    kLog.info(
        "tray pin removed token for id={} itemName='{}' title='{}' sniTitle='{}' icon='{}' process='{}' "
        "bus='{}'",
        item->id, item->itemName, item->title, item->statusNotifierTitle, item->iconName, item->processName,
        item->busName
    );
  } else {
    std::string token = tray::preferredPinToken(*item);
    if (token.empty()) {
      kLog.info(
          "tray pin skipped: no stable token for id={} itemName='{}' title='{}' sniTitle='{}' icon='{}' "
          "process='{}' bus='{}' objectPath='{}'",
          item->id, item->itemName, item->title, item->statusNotifierTitle, item->iconName, item->processName,
          item->busName, item->objectPath
      );
      return false;
    }
    kLog.info(
        "tray pin added token='{}' for id={} itemName='{}' title='{}' sniTitle='{}' icon='{}' process='{}' "
        "bus='{}'",
        token, item->id, item->itemName, item->title, item->statusNotifierTitle, item->iconName, item->processName,
        item->busName
    );
    pinned.push_back(token);
  }

  return m_config->setOverride({"widget", "tray", "pinned"}, pinned);
}

void TrayMenu::closeSubmenu() {
  if (m_submenuInstance != nullptr) {
    m_submenuInstance->inputDispatcher.setSceneRoot(nullptr);
    if (m_focusGrab != nullptr && m_submenuInstance->wlSurface != nullptr) {
      m_focusGrab->removeSurface(m_submenuInstance->wlSurface);
      m_focusGrab->commit();
    }
  }
  // Notify the server before clearing state so the parent id is still valid.
  if (m_tray != nullptr && !m_activeItemId.empty() && m_submenuParentEntryId != 0) {
    m_tray->notifyMenuClosed(m_activeItemId, m_submenuParentEntryId);
  }
  m_submenuInstance.reset();
  m_submenuEntries.clear();
  m_submenuParentEntryId = 0;
  m_pendingSubmenuParentEntryId = 0;
  m_pendingSubmenuRowCenterY = 0.0f;
}

void TrayMenu::openSubmenu(std::int32_t parentEntryId, float rowCenterY) {
  closeSubmenu();

  if (m_instance == nullptr || m_instance->surface == nullptr || m_tray == nullptr) {
    return;
  }

  m_submenuEntries = m_tray->menuEntriesForParent(m_activeItemId, parentEntryId);
  if (m_submenuEntries.empty()) {
    m_pendingSubmenuParentEntryId = parentEntryId;
    m_pendingSubmenuRowCenterY = rowCenterY;
    return;
  }
  m_pendingSubmenuParentEntryId = 0;
  m_pendingSubmenuRowCenterY = 0.0f;
  m_submenuParentEntryId = parentEntryId;
  // Signal the server that this submenu is being opened. Matches the opened/closed
  // pairing we do for the root menu.
  m_tray->notifyMenuOpened(m_activeItemId, parentEntryId);

  // Anchor rect is in the main popup's coordinate space (0,0 = top-left of main popup surface)
  const auto mainContentX = static_cast<std::int32_t>(std::lround(m_instance->chrome.contentX()));
  const auto mainWidth = static_cast<std::int32_t>(std::lround(m_instance->chrome.contentWidth));
  const auto mainX = m_instance->surface->configuredX() + mainContentX;
  const auto rowTop = static_cast<std::int32_t>(rowCenterY - Style::controlHeightSm * 0.5f);
  const auto rowH = static_cast<std::int32_t>(Style::controlHeightSm);
  constexpr std::int32_t kSubGap = 4;

  const auto chrome =
      popup_chrome::computeGeometry(kSurfaceWidth, static_cast<float>(submenuHeightPx()), popupShadowConfig(m_config));

  const auto* wlOutput = m_wayland->findOutputByWl(m_instance->output);
  const std::int32_t outputWidth = (wlOutput != nullptr && wlOutput->logicalWidth > 0)
                                       ? wlOutput->logicalWidth
                                       : static_cast<std::int32_t>(chrome.surfaceWidth);

  bool isRight = (m_instance->submenuDirection == ContextSubmenuDirection::Right);
  const std::int32_t submenuExtent = static_cast<std::int32_t>(chrome.surfaceWidth) + kSubGap;
  if (isRight) {
    if (mainX + mainWidth + submenuExtent > outputWidth) {
      isRight = false;
    }
  } else {
    if (mainX - submenuExtent < 0) {
      isRight = true;
    }
  }

  const std::int32_t anchorX = isRight ? mainContentX + mainWidth : mainContentX;
  const std::int32_t anchorY = static_cast<std::int32_t>(std::lround(m_instance->chrome.contentY())) + rowTop;
  const std::uint32_t anchor = isRight ? XDG_POSITIONER_ANCHOR_TOP_RIGHT : XDG_POSITIONER_ANCHOR_TOP_LEFT;
  const std::uint32_t gravity = isRight ? XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT : XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
  const std::int32_t offsetX = isRight ? kSubGap : -kSubGap;
  const auto subDir = isRight ? ContextSubmenuDirection::Right : ContextSubmenuDirection::Left;
  const auto chromeAttachment = isRight
                                    ? popup_chrome::Attachment{
                                          .horizontal = popup_chrome::HorizontalAttachment::Left,
                                          .vertical = popup_chrome::VerticalAttachment::Top,
                                      }
                                    : popup_chrome::Attachment{
                                          .horizontal = popup_chrome::HorizontalAttachment::Right,
                                          .vertical = popup_chrome::VerticalAttachment::Top,
                                      };

  auto inst = std::make_unique<MenuInstance>();
  inst->output = m_instance->output;
  inst->surface = std::make_unique<PopupSurface>(*m_wayland);
  inst->surface->setRenderContext(m_renderContext);
  inst->submenuDirection = subDir;
  inst->chrome = chrome;
  auto* instPtr = inst.get();

  inst->surface->setConfigureCallback([instPtr](uint32_t /*w*/, uint32_t /*h*/) { instPtr->surface->requestLayout(); });
  inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
    prepareSubmenuFrame(*instPtr, needsUpdate, needsLayout);
  });
  inst->surface->setDismissedCallback([this]() { closeSubmenu(); });

  auto popupConfig = PopupSurfaceConfig{
      .anchorX = anchorX,
      .anchorY = anchorY,
      .anchorWidth = 1,
      .anchorHeight = rowH,
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = anchor,
      .gravity = gravity,
      .constraintAdjustment = kPopupConstraintAdjust,
      .offsetX = offsetX,
      .offsetY = 0,
      .serial = m_wayland->lastInputSerial(),
      .grab = (m_focusGrab == nullptr),
  };
  popup_chrome::applyToConfig(popupConfig, chrome, chromeAttachment);

  xdg_surface* parentXdg = m_instance->surface->xdgSurface();
  if (!inst->surface->initializeAsChild(parentXdg, m_instance->output, popupConfig)) {
    kLog.debug("tray submenu: failed to create child popup surface");
    m_submenuEntries.clear();
    m_submenuParentEntryId = 0;
    return;
  }

  popup_chrome::setContentInputRegion(*inst->surface, inst->chrome);
  inst->wlSurface = inst->surface->wlSurface();
  m_submenuInstance = std::move(inst);

  if (m_focusGrab != nullptr && m_submenuInstance->wlSurface != nullptr) {
    m_focusGrab->addSurface(m_submenuInstance->wlSurface);
    m_focusGrab->commit();
  }
}

void TrayMenu::prepareSubmenuFrame(MenuInstance& inst, bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->width())) != width ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->height())) != height;
  if (needsSceneBuild || needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    buildSubmenuScene(inst, width, height);
  }
}

void TrayMenu::buildSubmenuScene(MenuInstance& inst, uint32_t width, uint32_t height) {
  uiAssertNotRendering("TrayMenu::buildSubmenuScene");
  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  inst.sceneRoot = std::make_unique<Node>();
  inst.sceneRoot->setSize(w, h);
  (void)popup_chrome::addShadow(*inst.sceneRoot, inst.chrome, popupShadowConfig(m_config), Style::scaledRadiusLg());

  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(m_submenuEntries.size());
  for (const auto& entry : m_submenuEntries) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = entry.id,
            .label = entry.label,
            .enabled = entry.enabled,
            .separator = entry.separator,
            .hasSubmenu = entry.hasSubmenu,
            .checkmark = entry.checkmark,
            .radio = entry.radio,
            .toggleState = entry.toggleState,
        }
    );
  }

  const float menuWidth = std::max(1.0f, inst.chrome.contentWidth);

  auto scrollView = std::make_unique<ScrollView>();
  scrollView->setPosition(inst.chrome.contentX(), inst.chrome.contentY());
  scrollView->setSize(inst.chrome.contentWidth, inst.chrome.contentHeight);
  scrollView->setViewportPaddingH(0.0f);
  scrollView->setViewportPaddingV(0.0f);
  scrollView->clearFill();
  scrollView->clearBorder();
  scrollView->setRadius(0.0f);
  scrollView->bindState(&inst.scrollState);
  scrollView->setScrollbarVisible(true);

  auto menu = std::make_unique<ContextMenuControl>();
  menu->setMenuWidth(menuWidth);
  menu->setMaxVisible(entries.size()); // Always lay out all entries for scrolling
  menu->setSubmenuDirection(inst.submenuDirection);
  menu->setEntries(std::move(entries));
  menu->setRedrawCallback([&inst]() {
    if (inst.surface != nullptr) {
      inst.surface->requestRedraw();
    }
  });
  menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
    if (m_tray == nullptr || m_activeItemId.empty()) {
      return;
    }
    DeferredCall::callLater([this, entry]() {
      if (m_tray != nullptr) {
        (void)m_tray->activateMenuEntry(m_activeItemId, entry.id);
      }
      close();
      closeTrayDrawerPanelIfOpen();
    });
  });
  scrollView->content()->addChild(std::move(menu));
  scrollView->layout(*m_renderContext);
  inst.sceneRoot->addChild(std::move(scrollView));

  inst.inputDispatcher.setSceneRoot(inst.sceneRoot.get());
  inst.inputDispatcher.setCursorShapeCallback([this](uint32_t serial, uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  inst.surface->setSceneRoot(inst.sceneRoot.get());
}
