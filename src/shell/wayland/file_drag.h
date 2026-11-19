#pragma once

#include "wayland/wayland_extension.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class WaylandConnection;
struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_buffer;
struct wl_data_source;
struct wl_surface;

// Drags files out of shell surfaces into other clients as a text/uri-list, and takes file drops onto them.
class FileDrag final : public WaylandExtension {
public:
  struct DropHandler {
    // Whether a file drop at surface-local (x, y) would be taken.
    std::function<bool(wl_surface* surface, double x, double y)> accepts;
    std::function<void(wl_surface* surface, double x, double y, std::vector<std::string> paths)> drop;
  };

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

  // Starts receiving file drops on shell surfaces.
  void setDropHandler(DropHandler handler);

  void onSend(wl_data_source* source, const char* mimeType, int fd);
  void onDataOffer(wl_data_offer* offer);
  void onOfferMime(wl_data_offer* offer, const char* mimeType);
  void onEnter(std::uint32_t serial, wl_surface* surface, double x, double y, wl_data_offer* offer);
  void onLeave();
  void onMotion(double x, double y);
  void onDrop();
  void onSelection(wl_data_offer* offer);
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

  DropHandler m_dropHandler;
  std::unordered_map<wl_data_offer*, bool> m_offerHasUris;
  wl_data_offer* m_dndOffer = nullptr;
  wl_surface* m_dndSurface = nullptr;
  std::uint32_t m_dndSerial = 0;
  double m_dndX = 0.0;
  double m_dndY = 0.0;
  bool m_dndAccepted = false;

  bool ensureDevice();
  void attachIcon(const std::string& iconPath);
  void endDrag();
  void updateDropAcceptance();
  void destroyOffer(wl_data_offer* offer);
  std::string readUriList(wl_data_offer* offer);
};
