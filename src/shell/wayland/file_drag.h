#pragma once

#include "wayland/wayland_extension.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class WaylandConnection;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_buffer;
struct wl_data_source;
struct wl_surface;

// Drags files out of shell surfaces into other clients as a text/uri-list.
class FileDrag final : public WaylandExtension {
public:
  explicit FileDrag(WaylandConnection& connection);
  ~FileDrag() override;

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  // Starts a drag of `paths` from `origin`; `serial` must belong to the pointer grab the drag starts from.
  // `iconPath` is drawn under the pointer while dragging; `onEnd` runs once the drop or cancel completes.
  bool start(
      wl_surface* origin, std::uint32_t serial, const std::vector<std::string>& paths, const std::string& iconPath,
      std::function<void()> onEnd
  );

  void onSend(wl_data_source* source, const char* mimeType, int fd);
  void onDropPerformed(wl_data_source* source);
  void onFinished(wl_data_source* source);

private:
  WaylandConnection& m_connection;
  wl_data_device_manager* m_manager = nullptr;
  wl_data_device* m_device = nullptr;
  wl_data_source* m_source = nullptr;
  wl_surface* m_icon = nullptr;
  wl_buffer* m_iconBuffer = nullptr;
  std::string m_uriList;
  std::function<void()> m_onEnd;

  void attachIcon(const std::string& iconPath);
  void endDrag();
};
