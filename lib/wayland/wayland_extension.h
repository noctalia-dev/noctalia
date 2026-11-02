#pragma once

#include <cstdint>
#include <string_view>

struct wl_registry;

// Binds registry globals that WaylandConnection does not bind itself. WaylandConnection::extension<T>()
// constructs T from the connection, owns it, and forwards registry events to it.
class WaylandExtension {
public:
  WaylandExtension() = default;
  virtual ~WaylandExtension() = default;

  WaylandExtension(const WaylandExtension&) = delete;
  WaylandExtension& operator=(const WaylandExtension&) = delete;

  // Called for every advertised global, after the connection bound its own globals for it.
  virtual void
  onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) = 0;
  virtual void onGlobalRemove(std::uint32_t /*name*/) {}
  // Destroys every protocol object the extension created. The connection calls this before it disconnects
  // the display; destructors run after the disconnect and must not issue requests.
  virtual void releaseGlobals() = 0;
};
