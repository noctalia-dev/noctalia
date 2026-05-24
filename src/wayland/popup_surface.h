#pragma once

#include "wayland/surface.h"

#include <cstdint>
#include <functional>

struct wl_output;
struct xdg_popup;
struct xdg_surface;
struct zwlr_layer_surface_v1;

struct PopupSurfaceConfig {
  std::int32_t anchorX = 0;
  std::int32_t anchorY = 0;
  std::int32_t anchorWidth = 1;
  std::int32_t anchorHeight = 1;
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::uint32_t anchor = 0;
  std::uint32_t gravity = 0;
  std::uint32_t constraintAdjustment = 0;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  std::uint32_t serial = 0;
  bool grab = true;
};

class PopupSurface : public Surface {
public:
  explicit PopupSurface(WaylandConnection& connection);
  ~PopupSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(zwlr_layer_surface_v1* parentLayerSurface, wl_output* output, PopupSurfaceConfig config);
  bool initializeAsChild(xdg_surface* parentXdgSurface, wl_output* output, PopupSurfaceConfig config);
  bool resize(std::uint32_t width, std::uint32_t height);

  void setDismissedCallback(std::function<void()> callback);

  [[nodiscard]] xdg_surface* xdgSurface() const noexcept { return m_xdgSurface; }
  [[nodiscard]] std::int32_t configuredX() const noexcept { return m_configuredX; }
  [[nodiscard]] std::int32_t configuredY() const noexcept { return m_configuredY; }

  static void handleXdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial);
  static void handlePopupConfigure(
      void* data, xdg_popup* popup, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height
  );
  static void handlePopupDone(void* data, xdg_popup* popup);
  static void handlePopupRepositioned(void* data, xdg_popup* popup, std::uint32_t token);

private:
  void destroyRoleObjects();
  // Decide whether to take xdg_popup_grab vs. enroll in the active focus_grab
  // host. Sets m_enrolledInGrabHost when we registered with the host.
  void wireGrab();
  void unenrollFromGrabHost();

  PopupSurfaceConfig m_config;
  xdg_surface* m_xdgSurface = nullptr;
  xdg_popup* m_popup = nullptr;
  std::function<void()> m_dismissedCallback;
  std::uint32_t m_pendingWidth = 0;
  std::uint32_t m_pendingHeight = 0;
  std::uint32_t m_repositionToken = 0;
  std::int32_t m_configuredX = 0;
  std::int32_t m_configuredY = 0;
  bool m_enrolledInGrabHost = false;
};
