#pragma once

#include "wayland/wayland_extension.h"

class WaylandConnection;
struct ext_background_effect_manager_v1;

// Binds ext_background_effect_manager_v1 for compositor blur behind shell surfaces.
class BackgroundEffectGlobals final : public WaylandExtension {
public:
  explicit BackgroundEffectGlobals(WaylandConnection& connection);

  void onGlobal(wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version) override;
  void releaseGlobals() override;

  ext_background_effect_manager_v1* manager() const noexcept { return m_manager; }
  // True once the compositor advertised the blur capability.
  bool hasBlur() const noexcept { return m_blurSupported; }

  void onCapabilities(std::uint32_t capabilities) noexcept;

private:
  ext_background_effect_manager_v1* m_manager = nullptr;
  bool m_blurSupported = false;
};
