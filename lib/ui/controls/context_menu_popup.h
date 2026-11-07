#pragma once

#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"
#include "ui/surface_shadow.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Node;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class ContextMenuPopupTestAccess;

struct ContextMenuPopupPlacement {
  std::uint32_t anchor = 0;
  std::uint32_t gravity = 0;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  popup_chrome::Attachment chromeAttachment{
      .horizontal = popup_chrome::HorizontalAttachment::Center,
      .vertical = popup_chrome::VerticalAttachment::Top,
  };
};

struct ContextMenuPopupRequest {
  std::vector<ContextMenuControlEntry> entries;
  // <= 0 sizes the menu to its widest entry, clamped to [minMenuWidth, maxMenuWidth] (0 = unbounded).
  float menuWidth = 0.0F;
  float minMenuWidth = 0.0F;
  float maxMenuWidth = 0.0F;
  // Scales row heights, fonts, and insets (select-style dropdowns derive this from their font size).
  float contentScale = 1.0F;
  std::size_t maxVisible = 0;
  // Entry highlighted on open; entries.size() leaves no highlight. Other out-of-range values use
  // the first interactive entry, including the default value.
  std::size_t initialHighlight = static_cast<std::size_t>(-1);
  PopupAnchorRect anchor;
  PopupSurfaceParent parent;
  // Surface whose pointer events are translated into menu coordinates while a press holds the
  // pointer capture (scrollbar thumb drags leaving the popup).
  wl_surface* pointerParentSurface = nullptr;
  // Serial of the input event that requested this popup. When absent, legacy
  // callers retain the previous last-input-serial behavior.
  std::optional<std::uint32_t> inputSerial = std::nullopt;
  std::optional<ContextMenuPopupPlacement> placement = std::nullopt;
  bool clearHighlightOnLeave = false;
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(ContextMenuPopupRequest request);
  void close();
  bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  void setShadowConfig(const ShadowStyle& shadow);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  wl_surface* wlSurface() const noexcept;
  xdg_surface* xdgSurface() const noexcept;
  std::uint32_t width() const noexcept;
  std::uint32_t height() const noexcept;

  // Route a keyboard event to the currently-open context menu, if any. A grab
  // popup is modal, so while one is open it swallows keys (returns true).
  static bool dispatchKeyboardEvent(const KeyboardEvent& event);

private:
  friend class ContextMenuPopupTestAccess;

  WaylandConnection& m_wayland;
  RenderContext& m_renderContext;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_sceneRoot;
  InputDispatcher m_inputDispatcher;
  ScrollViewState m_scrollState{};
  ScrollView* m_scrollView = nullptr;
  ContextMenuControl* m_menu = nullptr;
  std::size_t m_highlightedIndex = 0;
  wl_surface* m_wlSurface = nullptr;
  wl_surface* m_pointerParentSurface = nullptr;
  bool m_pointerInside = false;

  void restoreParentKeyboardInteractivity();
  void ensureHighlightedVisible();
  void requestVisualUpdate();
  void deferActivation(ContextMenuControlEntry entry);
  void deferClose();

  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void()> m_onDismissed;
  ShadowStyle m_shadowConfig;
  // Deferred popup callbacks must not dereference this after an owner (for
  // example, a plugin panel being unregistered) destroys the popup.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

  // Restores the parent's keyboard focus behaviour after parent.keyboardGrab; empty when unused.
  std::function<void()> m_releaseParentKeyboard;

  static ContextMenuPopup* s_openMenu;
};
