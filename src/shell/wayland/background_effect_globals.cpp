#include "shell/wayland/background_effect_globals.h"

#include "ext-background-effect-v1-client-protocol.h"

#include <algorithm>

namespace {

  constexpr std::uint32_t kExtBackgroundEffectManagerVersion = 1;
  // REMOVEME(wayland-protocols-1.45): Use the generated mask after requiring wayland-protocols >= 1.46;
  // version 1.45 generates zero for blur although the corrected v1 wire mask is 1.
  constexpr std::uint32_t kExtBackgroundEffectBlurCapabilityMask = 1U;

  void handleCapabilities(void* data, ext_background_effect_manager_v1* /*manager*/, std::uint32_t capabilities) {
    static_cast<BackgroundEffectGlobals*>(data)->onCapabilities(capabilities);
  }

  const ext_background_effect_manager_v1_listener kListener = {
      .capabilities = &handleCapabilities,
  };

} // namespace

BackgroundEffectGlobals::BackgroundEffectGlobals(WaylandConnection& /*connection*/) {}

void BackgroundEffectGlobals::onGlobal(
    wl_registry* registry, std::uint32_t name, std::string_view interface, std::uint32_t version
) {
  if (interface == ext_background_effect_manager_v1_interface.name) {
    const auto bindVersion = std::min(version, kExtBackgroundEffectManagerVersion);
    m_manager = static_cast<ext_background_effect_manager_v1*>(
        wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, bindVersion)
    );
    ext_background_effect_manager_v1_add_listener(m_manager, &kListener, this);
  }
}

void BackgroundEffectGlobals::releaseGlobals() {
  if (m_manager != nullptr) {
    ext_background_effect_manager_v1_destroy(m_manager);
    m_manager = nullptr;
    m_blurSupported = false;
  }
}

void BackgroundEffectGlobals::onCapabilities(std::uint32_t capabilities) noexcept {
  m_blurSupported = (capabilities & kExtBackgroundEffectBlurCapabilityMask) != 0U;
}
