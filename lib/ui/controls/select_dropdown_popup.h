#pragma once

#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_popup_context.h"
#include "ui/surface_shadow.h"

#include <cstdint>

class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;
struct xdg_surface;

// A Select's dropdown is a context menu: this adapter maps DropdownRequest onto the shared
// ContextMenuPopup / ContextMenuControl stack so rows, scrolling, and keyboard navigation have a
// single implementation. Owners hold one instance per surface and route events into it.
class SelectDropdownPopup : public SelectPopupContext {
public:
  SelectDropdownPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~SelectDropdownPopup() override;

  void setParent(PopupParentAttach attach, wl_surface* parentWlSurface, wl_output* output);
  void setParent(xdg_surface* xdgSurface, wl_surface* parentWlSurface, wl_output* output);
  void setShadowConfig(const ShadowStyle& shadow);

  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  wl_surface* wlSurface() const noexcept;
  xdg_surface* xdgSurface() const noexcept;
  std::uint32_t popupWidth() const noexcept;
  std::uint32_t popupHeight() const noexcept;

private:
  ContextMenuPopup m_popup;
  PopupParentAttach m_parentAttach;
  xdg_surface* m_parentXdgSurface = nullptr;
  wl_surface* m_parentWlSurface = nullptr;
  wl_output* m_parentOutput = nullptr;
};
