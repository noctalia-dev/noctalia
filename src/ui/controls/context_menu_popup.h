#pragma once

#include "config/config_types.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"

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
  float menuWidth = 0.0f;
  float minMenuWidth = 0.0f;
  float maxMenuWidth = 0.0f;
  // Scales row heights, fonts, and insets (select-style dropdowns derive this from their font size).
  float contentScale = 1.0f;
  std::size_t maxVisible = 0;
  // Entry highlighted (and scrolled into view) when the menu opens; out-of-range falls back to the
  // first interactive entry.
  std::size_t initialHighlight = static_cast<std::size_t>(-1);
  PopupAnchorRect anchor;
  PopupSurfaceParent parent;
  // Surface whose pointer events are translated into menu coordinates while a press holds the
  // pointer capture (scrollbar thumb drags leaving the popup). Defaults to parent.wlSurface.
  wl_surface* pointerParentSurface = nullptr;
  std::optional<ContextMenuPopupPlacement> placement = std::nullopt;
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(ContextMenuPopupRequest request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  void setShadowConfig(const ShellConfig::ShadowConfig& shadow);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] xdg_surface* xdgSurface() const noexcept;
  [[nodiscard]] std::uint32_t width() const noexcept;
  [[nodiscard]] std::uint32_t height() const noexcept;

  // Route a keyboard event to the currently-open context menu, if any. A grab
  // popup is modal, so while one is open it swallows keys (returns true).
  static bool dispatchKeyboardEvent(const KeyboardEvent& event);

private:
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

  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void()> m_onDismissed;
  ShellConfig::ShadowConfig m_shadowConfig;

  // Parent layer surface (e.g. a bar) whose keyboard interactivity is flipped to
  // OnDemand while the menu is open so the grabbing popup inherits keyboard
  // focus, then restored to None on close.
  zwlr_layer_surface_v1* m_keyboardParentLayerSurface = nullptr;
  wl_surface* m_keyboardParentWlSurface = nullptr;

  static ContextMenuPopup* s_openMenu;
};
