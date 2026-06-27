#include "shell/dock/dock_context_menu.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/pinned_apps.h"
#include "shell/surface/shadow.h"
#include "system/desktop_entry.h"
#include "ui/controls/context_menu.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_toplevels.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shell::dock {
  namespace {

    constexpr Logger kLog("dock");

    constexpr float kMenuWidth = 240.0f;
    constexpr std::int32_t kMenuCloseId = -1;
    constexpr std::int32_t kMenuCloseAllId = -2;
    constexpr std::int32_t kMenuSeparatorId = -3;
    constexpr std::int32_t kMenuPinToggleId = -4;
    constexpr std::int32_t kMenuWindowBaseId = -1000;

    bool isWindowClosable(const ToplevelInfo& window) {
      if (window.handle != nullptr) {
        return true;
      }
      if (compositors::isKde()) {
        return !window.identifier.empty() || (!window.title.empty() && !window.appId.empty());
      }
      return false;
    }

    popup_chrome::Attachment popupAttachmentForDockPosition(bool isBottom, bool isTop, bool isRight) {
      if (isBottom) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center,
            .vertical = popup_chrome::VerticalAttachment::Bottom
        };
      }
      if (isTop) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
        };
      }
      if (isRight) {
        return popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Right,
            .vertical = popup_chrome::VerticalAttachment::Center
        };
      }
      return popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Left, .vertical = popup_chrome::VerticalAttachment::Center
      };
    }

  } // namespace

  DockPopup::DockPopup() = default;
  DockPopup::~DockPopup() = default;

  bool routePopupEvent(DockPopup& popup, const PointerEvent& event) {
    const bool onPopup = (event.surface != nullptr && event.surface == popup.wlSurface);
    bool consumed = false;

    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onPopup) {
        popup.pointerInside = true;
        popup.inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (onPopup) {
        popup.pointerInside = false;
        popup.inputDispatcher.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (onPopup || popup.pointerInside) {
        if (onPopup) {
          popup.pointerInside = true;
        }
        popup.inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
        consumed = true;
      }
      break;
    case PointerEvent::Type::Button:
      if (onPopup || popup.pointerInside) {
        if (onPopup) {
          popup.pointerInside = true;
        }
        // Keep hover state synced before click dispatch so stationary pointers can
        // still activate rows even if Enter/Motion ordering is flaky.
        popup.inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
        const bool pressed = (event.state == 1);
        popup.inputDispatcher.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
        );
        consumed = true;
      }
      break;
    case PointerEvent::Type::Axis:
      break;
    }

    if (popup.surface != nullptr
        && popup.sceneRoot != nullptr
        && (popup.sceneRoot->paintDirty() || popup.sceneRoot->layoutDirty())) {
      if (popup.sceneRoot->layoutDirty()) {
        popup.surface->requestLayout();
      } else {
        popup.surface->requestRedraw();
      }
    }

    return consumed;
  }

  std::unique_ptr<DockPopup> createItemMenu(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, const DockConfig& dockConfig,
      const DesktopEntry& entry, const std::vector<ToplevelInfo>& windows, const DockMenuCallbacks& callbacks
  ) {
    auto menu = std::make_unique<DockPopup>();

    // Collect running windows for activation/close actions.
    menu->windows = windows;

    // IDs 0..N-1 -> desktop actions; negative constants -> windows / close commands.
    std::vector<ContextMenuControlEntry> entries;
    entries.reserve(windows.size() + entry.actions.size() + 5);

    const bool isPinned = pinned_apps::containsEntry(dockConfig.pinned, entry);
    entries.push_back(
        ContextMenuControlEntry{
            .id = kMenuPinToggleId,
            .label = i18n::tr(isPinned ? "tray.menu.unpin" : "tray.menu.pin"),
            .enabled = callbacks.setEntryPinned != nullptr,
            .separator = false,
            .hasSubmenu = false,
        }
    );

    std::vector<ContextMenuControlEntry> bodyEntries;
    bodyEntries.reserve(windows.size() + entry.actions.size() + 4);

    for (std::size_t i = 0; i < windows.size(); ++i) {
      const auto& title = windows[i].title.empty() ? entry.name : windows[i].title;
      bodyEntries.push_back(
          ContextMenuControlEntry{
              .id = kMenuWindowBaseId - static_cast<std::int32_t>(i),
              .label = title,
              .enabled = windows[i].handle != nullptr
                  || !windows[i].identifier.empty()
                  || (compositors::isKde() && (!windows[i].title.empty() || !windows[i].appId.empty())),
              .separator = false,
              .hasSubmenu = false,
          }
      );
    }

    const bool hasWindowEntries = !windows.empty();
    const bool hasActionEntries = !entry.actions.empty();
    std::vector<std::size_t> closableWindowIndices;
    closableWindowIndices.reserve(menu->windows.size());
    for (std::size_t i = 0; i < menu->windows.size(); ++i) {
      if (isWindowClosable(menu->windows[i])) {
        closableWindowIndices.push_back(i);
      }
    }
    const std::size_t closableCount = closableWindowIndices.size();
    const bool hasCloseEntries = closableCount > 0;
    if (hasWindowEntries && (hasActionEntries || hasCloseEntries)) {
      bodyEntries.push_back(
          ContextMenuControlEntry{
              .id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false
          }
      );
    }

    for (std::int32_t i = 0; i < static_cast<std::int32_t>(entry.actions.size()); ++i) {
      bodyEntries.push_back(
          ContextMenuControlEntry{
              .id = i,
              .label = entry.actions[static_cast<std::size_t>(i)].name,
              .enabled = true,
              .separator = false,
              .hasSubmenu = false,
          }
      );
    }

    if (closableCount > 0) {
      if (hasActionEntries) {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false
            }
        );
      }
      if (closableCount == 1) {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuCloseId,
                .label = i18n::tr("dock.actions.close"),
                .enabled = true,
                .separator = false,
                .hasSubmenu = false
            }
        );
      } else {
        bodyEntries.push_back(
            ContextMenuControlEntry{
                .id = kMenuCloseAllId,
                .label = i18n::tr("dock.actions.close-all"),
                .enabled = true,
                .separator = false,
                .hasSubmenu = false
            }
        );
      }
    }

    if (!bodyEntries.empty()) {
      entries.push_back(
          ContextMenuControlEntry{
              .id = kMenuSeparatorId, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false
          }
      );
      entries.insert(entries.end(), bodyEntries.begin(), bodyEntries.end());
    }

    if (entries.empty()) {
      return nullptr;
    }

    // Compute popup height.
    const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size());

    // Determine anchor / gravity + gap based on dock position.
    const DockEdge edge = dockConfig.position;
    const bool isBottom = edge == DockEdge::Bottom;
    const bool isTop = edge == DockEdge::Top;
    const bool isRight = edge == DockEdge::Right;

    std::uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
    std::uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
    std::int32_t offsetX = 0;
    std::int32_t offsetY = 0;
    const std::int32_t kGapBottom = std::max(2, static_cast<std::int32_t>(Style::spaceLg));
    const std::int32_t kGap = std::max(2, static_cast<std::int32_t>(Style::spaceMd));

    if (isBottom) {
      anchor = XDG_POSITIONER_ANCHOR_TOP;
      gravity = XDG_POSITIONER_GRAVITY_TOP;
      offsetY = -kGapBottom;
    } else if (isTop) {
      anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
      gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
      offsetY = kGap;
    } else if (isRight) {
      anchor = XDG_POSITIONER_ANCHOR_LEFT;
      gravity = XDG_POSITIONER_GRAVITY_LEFT;
      offsetX = -kGap;
    } else { // left
      anchor = XDG_POSITIONER_ANCHOR_RIGHT;
      gravity = XDG_POSITIONER_GRAVITY_RIGHT;
      offsetX = kGap;
    }

    const auto sb = shell::surface_shadow::bleed(dockConfig.shadow, config.config().shell.shadow);
    const std::int32_t panelThk = shell::dock::dockThickness(dockConfig);
    const auto ptrX = static_cast<std::int32_t>(platform.lastPointerX());
    const auto ptrY = static_cast<std::int32_t>(platform.lastPointerY());
    const std::int32_t halfCell = dockConfig.iconSize / 2;

    // Anchor rect: pointer-centred on main axis x panel face on cross axis.
    std::int32_t aX, aY, aW, aH;
    if (isBottom) {
      // Panel top face is at sb.up.
      aX = ptrX - halfCell;
      aY = sb.up;
      aW = halfCell * 2;
      aH = panelThk;
    } else if (isTop) {
      const std::int32_t panelFace = std::min(dockConfig.marginEdge, sb.up) + panelThk;
      aX = ptrX - halfCell;
      aY = 0;
      aW = halfCell * 2;
      aH = panelFace;
    } else if (isRight) {
      aX = sb.left;
      aY = ptrY - halfCell;
      aW = panelThk;
      aH = halfCell * 2;
    } else { // left
      const std::int32_t panelFace = std::min(dockConfig.marginEdge, sb.left) + panelThk;
      aX = 0;
      aY = ptrY - halfCell;
      aW = panelFace;
      aH = halfCell * 2;
    }

    const auto menuChrome = popup_chrome::computeGeometry(kMenuWidth, menuHeight, config.config().shell.shadow);
    PopupSurfaceConfig popupCfg{
        .anchorX = aX,
        .anchorY = aY,
        .anchorWidth = std::max(1, aW),
        .anchorHeight = std::max(1, aH),
        .width = menuChrome.surfaceWidth,
        .height = menuChrome.surfaceHeight,
        .anchor = anchor,
        .gravity = gravity,
        .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
        .offsetX = offsetX,
        .offsetY = offsetY,
        .serial = platform.lastInputSerial(),
        .grab = true,
    };
    popup_chrome::applyToConfig(popupCfg, menuChrome, popupAttachmentForDockPosition(isBottom, isTop, isRight));

    menu->surface = std::make_unique<PopupSurface>(platform.wayland());
    menu->surface->setRenderContext(&renderContext);
    menu->chrome = menuChrome;

    auto* menuPtr = menu.get();

    // Capture actions by value; the entry may be rebuilt before the callback fires.
    auto entryActions = entry.actions;

    menu->surface->setConfigureCallback([menuPtr](std::uint32_t /*w*/, std::uint32_t /*h*/) {
      menuPtr->surface->requestLayout();
    });
    menu->surface->setPrepareFrameCallback([&platform, &config, &renderContext, menuPtr, entries, entryActions,
                                            callbacks, isPinned,
                                            closableWindowIndices](bool /*needsUpdate*/, bool needsLayout) {
      if (menuPtr->surface == nullptr) {
        return;
      }

      const auto width = menuPtr->surface->width();
      const auto height = menuPtr->surface->height();
      if (width == 0 || height == 0) {
        return;
      }

      renderContext.makeCurrent(menuPtr->surface->renderTarget());

      const bool needsSceneBuild = menuPtr->sceneRoot == nullptr
          || static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->width())) != width
          || static_cast<std::uint32_t>(std::round(menuPtr->sceneRoot->height())) != height;
      if (!needsSceneBuild && !needsLayout) {
        return;
      }

      UiPhaseScope layoutPhase(UiPhase::Layout);

      const auto fw = static_cast<float>(width);
      const auto fh = static_cast<float>(height);

      menuPtr->sceneRoot = std::make_unique<Node>();
      menuPtr->sceneRoot->setSize(fw, fh);
      (void)popup_chrome::addShadow(
          *menuPtr->sceneRoot, menuPtr->chrome, config.config().shell.shadow, Style::scaledRadiusLg()
      );

      auto ctrl = std::make_unique<ContextMenuControl>();
      ctrl->setMenuWidth(menuPtr->chrome.contentWidth);
      ctrl->setMaxVisible(entries.size());
      ctrl->setEntries(entries);
      ctrl->setRedrawCallback([menuPtr]() {
        if (menuPtr->surface)
          menuPtr->surface->requestRedraw();
      });
      ctrl->setOnActivate([menuPtr, entryActions, callbacks, isPinned,
                           closableWindowIndices](const ContextMenuControlEntry& e) {
        const std::int32_t id = e.id;
        auto menuWindows = menuPtr->windows;
        DeferredCall::callLater([id, entryActions, callbacks, isPinned, menuWindows = std::move(menuWindows),
                                 closableWindowIndices]() mutable {
          if (id == kMenuPinToggleId) {
            if (callbacks.setEntryPinned) {
              callbacks.setEntryPinned(!isPinned);
            }
          } else if (id <= kMenuWindowBaseId) {
            const auto idx = static_cast<std::size_t>(kMenuWindowBaseId - id);
            if (idx < menuWindows.size() && callbacks.activateWindow) {
              callbacks.activateWindow(idx);
            }
          } else if (id >= 0) {
            const auto idx = static_cast<std::size_t>(id);
            if (idx < entryActions.size() && callbacks.launchAction) {
              callbacks.launchAction(entryActions[idx]);
            }
          } else if (id == kMenuCloseId && !closableWindowIndices.empty()) {
            if (callbacks.closeWindow) {
              callbacks.closeWindow(closableWindowIndices[0]);
            }
          } else if (id == kMenuCloseAllId) {
            if (callbacks.closeWindow) {
              for (std::size_t windowIndex : closableWindowIndices) {
                if (windowIndex < menuWindows.size()) {
                  callbacks.closeWindow(windowIndex);
                }
              }
            }
          }
          if (callbacks.closeMenu) {
            callbacks.closeMenu();
          }
        });
      });
      ctrl->setPosition(menuPtr->chrome.contentX(), menuPtr->chrome.contentY());
      ctrl->setSize(menuPtr->chrome.contentWidth, menuPtr->chrome.contentHeight);
      ctrl->layout(renderContext);

      menuPtr->sceneRoot->addChild(std::move(ctrl));
      menuPtr->inputDispatcher.setSceneRoot(menuPtr->sceneRoot.get());
      menuPtr->inputDispatcher.setCursorShapeCallback([&platform](std::uint32_t serial, std::uint32_t shape) {
        platform.setCursorShape(serial, shape);
      });
      menuPtr->surface->setSceneRoot(menuPtr->sceneRoot.get());
    });

    menu->surface->setDismissedCallback([callbacks]() {
      DeferredCall::callLater([callbacks]() {
        if (callbacks.closeMenu) {
          callbacks.closeMenu();
        }
      });
    });

    if (parentLayerSurface == nullptr || !menu->surface->initialize(parentLayerSurface, output, popupCfg)) {
      kLog.warn("dock: failed to create item-menu popup");
      return nullptr;
    }

    popup_chrome::setContentInputRegion(*menu->surface, menu->chrome);
    menu->wlSurface = menu->surface->wlSurface();
    return menu;
  }

} // namespace shell::dock
