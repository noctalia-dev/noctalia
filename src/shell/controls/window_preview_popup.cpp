#include "shell/controls/window_preview_popup.h"

#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "shell/switcher/window_switcher_tile.h"
#include "shell/wayland/foreign_toplevels.h"
#include "shell/wayland/layer_shell.h"
#include "shell/wayland/layer_surface.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace shell {
  namespace {

    constexpr Logger kLog("window-preview");
    constexpr std::size_t kMaxCardsPerPage = 12;
    constexpr std::size_t kMaxSnapshots = 24;
    constexpr int kMaxThumbnailWidth = 640;
    constexpr int kMaxThumbnailHeight = 360;

    bool sameWindow(const ToplevelInfo& a, const ToplevelInfo& b) {
      if (!a.identifier.empty()
          && !b.identifier.empty()
          && !toplevel_identity::isWlr(a.identifier)
          && !toplevel_identity::isWlr(b.identifier)) {
        return a.identifier == b.identifier && a.exactIdentity == b.exactIdentity;
      }
      return a.order == b.order
          && ((a.handle != nullptr && a.handle == b.handle) || (a.extHandle != nullptr && a.extHandle == b.extHandle));
    }

    bool canActOnWindow(const ToplevelInfo& window) {
      return window.handle != nullptr || (!window.identifier.empty() && (window.exactIdentity || compositors::isKde()));
    }

    bool isFocused(
        const ToplevelInfo& window, const std::optional<ActiveToplevel>& active,
        const std::optional<std::string>& focusedId
    ) {
      return (window.exactIdentity && !window.identifier.empty() && focusedId == window.identifier)
          || (active
              && ((window.handle != nullptr && active->handle == window.handle)
                  || (!window.identifier.empty() && active->identifier == window.identifier)));
    }

  } // namespace

  WindowPreviewPopup::WindowPreviewPopup(
      CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext, std::function<void()> dismiss
  )
      : m_platform(platform), m_config(config), m_renderContext(renderContext), m_dismiss(std::move(dismiss)),
        m_capture(platform.wayland()) {
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform.setCursorShape(serial, shape);
    });
  }

  WindowPreviewPopup::~WindowPreviewPopup() = default;

  bool WindowPreviewPopup::show(
      Anchor anchor, const InputArea& target, std::string appLabel, std::string iconPath, WindowsProvider windows
  ) {
    m_anchor.emplace(anchor);
    m_vertical = anchor.edge == DockEdge::Left || anchor.edge == DockEdge::Right;
    float x = 0.0F;
    float y = 0.0F;
    Node::mapToScene(&target, target.width() * 0.5F, target.height() * 0.5F, x, y);
    m_anchorMain = m_vertical ? y : x;
    ++m_appGeneration;
    cancelCaptures();
    m_snapshots.clear();
    m_appLabel = std::move(appLabel);
    m_iconPath = std::move(iconPath);
    m_getWindows = std::move(windows);
    m_windows.clear();
    m_page = 0;
    return refresh();
  }

  bool WindowPreviewPopup::refresh() {
    auto windows = m_getWindows();
    if (windows.empty()) {
      return false;
    }

    const bool scaleChanged = m_scale != m_config.config().accessibility.uiScale;
    if (scaleChanged) {
      cancelCaptures();
      m_snapshots.clear();
    }
    const auto active = m_platform.activeToplevel();
    const auto focusedId = m_platform.focusedCompositorWindowId();
    const bool sameMembership = std::ranges::equal(m_windows, windows, sameWindow);
    if (m_captureWindow && !std::ranges::any_of(windows, [&](const ToplevelInfo& window) {
          return sameWindow(*m_captureWindow, window)
              && m_captureWindow->extHandle == window.extHandle
              && !(window.minimizeState && window.minimizeState->minimized);
        })) {
      cancelCaptures();
    }
    std::erase_if(m_snapshots, [&](const Snapshot& snapshot) {
      return !std::ranges::any_of(windows, [&](const ToplevelInfo& window) {
        return sameWindow(snapshot.window, window) && snapshot.window.extHandle == window.extHandle;
      });
    });
    const bool unchanged =
        sameMembership && std::ranges::equal(m_windows, windows, [&](const ToplevelInfo& a, const ToplevelInfo& b) {
          return a.title == b.title
              && a.extHandle == b.extHandle
              && a.minimizeState == b.minimizeState
              && isFocused(a, m_active, m_focusedId) == isFocused(b, active, focusedId);
        });
    m_windows = std::move(windows);
    m_active = active;
    m_focusedId = focusedId;
    if ((!sameMembership || scaleChanged) && !updateGeometry()) {
      return false;
    }
    if (!unchanged || scaleChanged) {
      m_dirty = true;
      m_surface->requestLayout();
    }
    return true;
  }

  PopupSurfaceConfig WindowPreviewPopup::geometry() {
    const auto edge = m_anchor->edge;
    const auto& shadow = m_config.config().shell.shadow;
    m_scale = m_config.config().accessibility.uiScale;
    const float space = Style::spaceSm * m_scale;
    const auto bleed = popup_chrome::computeGeometry(1.0F, 1.0F, shadow, Style::popupShadowsEnabled()).bleed;
    const auto surfaceW = static_cast<float>(m_anchor->parent.width());
    const auto surfaceH = static_cast<float>(m_anchor->parent.height());
    const auto* output = m_platform.findOutputByWl(m_anchor->output);
    const float edgeClearance = m_anchor->edgeClearance + 2 * space;
    const float maxW = static_cast<float>((output ? output->effectiveLogicalWidth() : 0) - bleed.left - bleed.right)
        - (m_vertical ? edgeClearance : 0.0F);
    const float maxH = static_cast<float>((output ? output->effectiveLogicalHeight() : 0) - bleed.up - bleed.down)
        - (m_vertical ? 0.0F : edgeClearance);

    m_cardWidth = std::floor(std::min(static_cast<float>(m_anchor->cardWidth) * m_scale, maxW - 2 * space));
    m_cardHeight =
        std::floor(std::min(m_cardWidth * 0.5625F + Style::windowSwitcherCaptionHeight * m_scale, maxH - 2 * space));

    const float maxMain = m_vertical ? maxH : maxW;
    const auto capacity = [&](float footer) {
      const float cardMain = m_vertical ? m_cardHeight : m_cardWidth;
      const float fit = std::floor((maxMain - space - footer) / (std::max(1.0F, cardMain) + space));
      return static_cast<std::size_t>(std::clamp(fit, 1.0F, static_cast<float>(kMaxCardsPerPage)));
    };
    m_pageSize = capacity(0.0F);
    const bool paged = m_windows.size() > m_pageSize;
    const float footer = paged ? Style::controlHeightSm * m_scale + space : 0.0F;
    if (paged) {
      m_cardHeight = std::min(m_cardHeight, maxH - 2 * space - footer);
      if (m_vertical) {
        m_pageSize = capacity(footer);
      }
    }
    m_page = std::min(m_page, (m_windows.size() - 1) / m_pageSize);
    const auto n = static_cast<float>(std::min(m_windows.size(), m_pageSize));
    const float w = m_vertical ? m_cardWidth + 2 * space : n * (m_cardWidth + space) + space;
    const float h = (m_vertical ? n * (m_cardHeight + space) + space : m_cardHeight + 2 * space) + footer;
    m_chrome = popup_chrome::computeGeometry(w, h, shadow, Style::popupShadowsEnabled());

    const auto main = static_cast<std::int32_t>(
        std::clamp(m_anchorMain, 0.0F, std::max(0.0F, (m_vertical ? surfaceH : surfaceW) - 1.0F))
    );
    const auto cross = static_cast<std::int32_t>(
        std::clamp(m_anchor->inwardFace, 0.0F, std::max(0.0F, (m_vertical ? surfaceW : surfaceH) - 1.0F))
    );
    const auto gap = std::max(2, static_cast<std::int32_t>(space));
    PopupSurfaceConfig cfg{
        .constraintAdjustment =
            XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
        .grab = false,
    };
    popup_chrome::Attachment attachment;
    if (m_vertical) {
      const bool left = edge == DockEdge::Left;
      cfg.anchorX = cross;
      cfg.anchorY = main;
      cfg.anchor = left ? XDG_POSITIONER_ANCHOR_RIGHT : XDG_POSITIONER_ANCHOR_LEFT;
      cfg.gravity = left ? XDG_POSITIONER_GRAVITY_RIGHT : XDG_POSITIONER_GRAVITY_LEFT;
      cfg.offsetX = left ? gap : -gap;
      attachment.horizontal =
          left ? popup_chrome::HorizontalAttachment::Left : popup_chrome::HorizontalAttachment::Right;
      attachment.vertical = popup_chrome::VerticalAttachment::Center;
    } else {
      const bool top = edge == DockEdge::Top;
      cfg.anchorX = main;
      cfg.anchorY = cross;
      cfg.anchor = top ? XDG_POSITIONER_ANCHOR_BOTTOM : XDG_POSITIONER_ANCHOR_TOP;
      cfg.gravity = top ? XDG_POSITIONER_GRAVITY_BOTTOM : XDG_POSITIONER_GRAVITY_TOP;
      cfg.offsetY = top ? gap : -gap;
      attachment.vertical = top ? popup_chrome::VerticalAttachment::Top : popup_chrome::VerticalAttachment::Bottom;
    }
    popup_chrome::applyToConfig(cfg, m_chrome, attachment);
    return cfg;
  }

  bool WindowPreviewPopup::updateGeometry() {
    const auto cfg = geometry();
    if (m_cardWidth < 64.0F || m_cardHeight < Style::windowSwitcherCaptionHeight * m_scale + 24.0F) {
      return false;
    }
    if (m_surface != nullptr && m_surface->reposition(cfg)) {
      popup_chrome::setContentInputRegion(*m_surface, m_chrome);
      return true;
    }

    m_input.setSceneRoot(nullptr);
    m_sceneRoot.reset();
    m_tiles.clear();
    m_pointerInside = false;
    m_surface = std::make_unique<PopupSurface>(m_platform.wayland());
    m_surface->setRenderContext(&m_renderContext);
    m_surface->setConfigureCallback([this](std::uint32_t width, std::uint32_t height) {
      if (!m_sceneRoot
          || m_sceneRoot->width() != static_cast<float>(width)
          || m_sceneRoot->height() != static_cast<float>(height)) {
        m_dirty = true;
        m_surface->requestLayout();
      }
    });
    m_surface->setPrepareFrameCallback([this](bool, bool) { prepareFrame(); });
    m_surface->setDismissedCallback([this] {
      DeferredCall::callLater([life = std::weak_ptr(m_lifetime), dismiss = m_dismiss] {
        if (!life.expired()) {
          dismiss();
        }
      });
    });
    if (!m_surface->initialize(
            LayerShell::popupAttach(m_anchor->parent.layerSurface()), m_anchor->output, cfg, false
        )) {
      return false;
    }
    popup_chrome::setContentInputRegion(*m_surface, m_chrome);
    return true;
  }

  void WindowPreviewPopup::prepareFrame() {
    if ((!m_dirty && !m_thumbnailDirty)
        || m_surface->width() != m_chrome.surfaceWidth
        || m_surface->height() != m_chrome.surfaceHeight) {
      return;
    }
    const bool updateThumbnails = std::exchange(m_thumbnailDirty, false);
    const auto first = m_page * m_pageSize;
    const auto count = std::min(m_pageSize, m_windows.size() - first);
    m_renderContext.makeCurrent(m_surface->renderTarget());
    UiPhaseScope layoutPhase(UiPhase::Layout);
    auto& renderer = m_surface->renderTarget().renderer();
    if (!std::exchange(m_dirty, false)) {
      if (updateThumbnails) {
        for (std::size_t i = 0; i < count; ++i) {
          if (const auto* snapshot = findSnapshot(m_windows[first + i]); snapshot != nullptr && snapshot->image) {
            m_tiles[i]->setThumbnail(renderer, snapshot->image);
            m_tiles[i]->layout(renderer);
          }
        }
      }
      captureNext();
      return;
    }

    m_input.setSceneRoot(nullptr);
    m_sceneRoot = ui::node({});
    m_tiles.clear();
    m_sceneRoot->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
    const auto& shadow = m_config.config().shell.shadow;
    if (Style::popupShadowsEnabled()) {
      popup_chrome::addShadow(*m_sceneRoot, m_chrome, shadow, Style::scaledRadiusLg(m_scale));
    }
    popup_chrome::addCardBackground(*m_sceneRoot, m_chrome, m_scale);

    const float space = Style::spaceSm * m_scale;
    const float startX = m_chrome.contentX() + space;
    const float startY = m_chrome.contentY() + space;
    m_tiles.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const auto& window = m_windows[first + i];
      auto tile = std::make_unique<WindowSwitcherTile>(m_scale, nullptr);
      tile->setCardSize(m_cardWidth, m_cardHeight);
      tile->setIntegratedCaption(true);
      tile->setShowAppIconOnThumbnail(false);
      tile->setPosition(
          startX + (m_vertical ? 0.0F : static_cast<float>(i) * (m_cardWidth + space)),
          startY + (m_vertical ? static_cast<float>(i) * (m_cardHeight + space) : 0.0F)
      );
      tile->setOnInvalidate([this] { m_surface->requestRedraw(); });
      if (canActOnWindow(window)) {
        tile->setOnActivate([this, window] { queueWindowAction(window, false); });
        tile->setOnClose([this, window] { queueWindowAction(window, true); });
      }
      const auto* snapshot = findSnapshot(window);
      const WindowSwitcherEntry entry{
          .title = window.title,
          .appId = window.appId,
          .appLabel = m_appLabel,
          .iconPath = m_iconPath,
          .thumbnail = snapshot != nullptr ? snapshot->image : nullptr,
          .minimizeState = window.minimizeState,
      };
      tile->bind(
          renderer, entry,
          isFocused(window, m_active, m_focusedId) ? WindowSwitcherTileDepth::Selected : WindowSwitcherTileDepth::Near,
          true, true, WindowSwitcherHorizontalPlacement::Left, WindowSwitcherHorizontalPlacement::Right
      );
      tile->layout(renderer);
      m_tiles.push_back(tile.get());
      m_sceneRoot->addChild(std::move(tile));
    }
    if (m_windows.size() > m_pageSize) {
      const float buttonSize = Style::controlHeightSm * m_scale;
      const float y = m_chrome.contentBottom() - space - buttonSize;
      const float mid = m_chrome.contentX() + m_chrome.contentWidth * 0.5F;
      for (const int delta : {-1, 1}) {
        auto button = ui::button({
            .glyph = delta < 0 ? "chevron-left" : "chevron-right",
            .controlHeight = buttonSize,
            .width = buttonSize,
            .height = buttonSize,
            .onClick = [this, delta] { changePage(delta); },
        });
        button->setEnabled(delta < 0 ? m_page > 0 : first + count < m_windows.size());
        button->setPosition(delta < 0 ? mid - space - buttonSize : mid + space, y);
        button->layout(renderer);
        m_sceneRoot->addChild(std::move(button));
      }
    }
    m_input.setSceneRoot(m_sceneRoot.get());
    m_surface->setSceneRoot(m_sceneRoot.get());
    captureNext();
  }

  const WindowPreviewPopup::Snapshot* WindowPreviewPopup::findSnapshot(const ToplevelInfo& window) const {
    const auto found = std::ranges::find_if(m_snapshots, [&](const Snapshot& snapshot) {
      return sameWindow(snapshot.window, window) && snapshot.window.extHandle == window.extHandle;
    });
    return found == m_snapshots.end() ? nullptr : &*found;
  }

  void WindowPreviewPopup::cancelCaptures() {
    m_capture.cancelInFlight();
    m_captureWindow.reset();
  }

  void WindowPreviewPopup::captureNext() {
    if (!m_capture.available() || m_capture.busy()) {
      return;
    }
    const auto first = m_windows.begin() + static_cast<std::ptrdiff_t>(m_page * m_pageSize);
    const auto last = first + std::min(static_cast<std::ptrdiff_t>(m_pageSize), m_windows.end() - first);
    const auto target = std::find_if(first, last, [&](const ToplevelInfo& window) {
      if (window.extHandle == nullptr
          || (window.minimizeState && window.minimizeState->minimized)
          || findSnapshot(window) != nullptr) {
        return false;
      }
      // The capture handle may disappear before the dock's next model refresh.
      bool live = false;
      m_platform.foreignToplevels().visitExtToplevelHandles([&](ext_foreign_toplevel_handle_v1* handle) {
        live |= handle == window.extHandle;
      });
      return live;
    });
    if (target == last) {
      return;
    }
    const auto* output = m_platform.wayland().findOutputByWl(m_anchor->output);
    const float pixelScale = output != nullptr ? output->configuredScale() : 1.0F;
    const int width = std::clamp(static_cast<int>(std::ceil(m_cardWidth * pixelScale)), 1, kMaxThumbnailWidth);
    const int height = std::clamp(static_cast<int>(std::ceil(m_cardHeight * pixelScale)), 1, kMaxThumbnailHeight);
    m_captureWindow = *target;
    m_capture.capture(
        target->extHandle, width, height, [this](std::optional<ScreencopyImage> image, std::string error) {
          if (!error.empty()) {
            kLog.debug("thumbnail capture skipped: {}", error);
          }
          if (m_snapshots.size() == kMaxSnapshots) {
            m_snapshots.pop_front();
          }
          m_snapshots.push_back({
              .window = std::move(*m_captureWindow),
              .image = image ? std::make_shared<ScreencopyImage>(std::move(*image)) : nullptr,
          });
          m_captureWindow.reset();
          m_thumbnailDirty = true;
          m_surface->requestUpdateOnly();
        }
    );
  }

  void WindowPreviewPopup::changePage(int delta) {
    const auto last = (m_windows.size() - 1) / m_pageSize;
    const auto next = delta < 0 ? (m_page > 0 ? m_page - 1 : 0) : std::min(m_page + 1, last);
    if (next != m_page) {
      cancelCaptures();
      m_page = next;
      m_dirty = true;
      m_surface->requestLayout();
    }
  }

  void WindowPreviewPopup::queueWindowAction(const ToplevelInfo& window, bool close) {
    DeferredCall::callLater([this, life = std::weak_ptr(m_lifetime), generation = m_appGeneration, window, close] {
      if (life.expired() || generation != m_appGeneration) {
        return;
      }
      const auto windows = m_getWindows();
      const auto live =
          std::ranges::find_if(windows, [&](const auto& candidate) { return sameWindow(window, candidate); });
      if (live == windows.end() || !canActOnWindow(*live)) {
        return;
      }
      if (close) {
        m_platform.closeToplevelInfo(*live);
      } else {
        auto& platform = m_platform;
        auto dismiss = m_dismiss;
        dismiss();
        platform.activateToplevelInfo(*live);
      }
    });
  }

  bool WindowPreviewPopup::routePointer(const PointerEvent& event) {
    if (event.surface == nullptr || event.surface != m_surface->wlSurface()) {
      return false;
    }
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_pointerInside = true;
      m_input.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_pointerInside = false;
      m_input.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_input.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
      break;
    case PointerEvent::Type::Axis:
      if (event.axisValue != 0.0) {
        changePage(event.axisValue < 0.0 ? -1 : 1);
      }
      break;
    }
    return true;
  }

} // namespace shell
