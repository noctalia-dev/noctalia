#include "compositors/compositor_runtime.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/mango/mango_runtime.h"
#include "compositors/niri/niri_runtime.h"
#include "compositors/sway/sway_runtime.h"
#include "compositors/triad/triad_runtime.h"
#include "compositors/umbriel/umbriel_runtime.h"

namespace compositors {

  CompositorRuntimeRegistry::CompositorRuntimeRegistry()
      : m_hyprland(std::make_unique<hyprland::HyprlandRuntime>()), m_mango(std::make_unique<mango::MangoRuntime>()),
        m_niri(std::make_unique<niri::NiriRuntime>()), m_sway(std::make_unique<sway::SwayRuntime>()),
        m_triad(std::make_unique<triad::TriadRuntime>()), m_umbriel(std::make_unique<umbriel::UmbrielRuntime>()) {}

  CompositorRuntimeRegistry::~CompositorRuntimeRegistry() = default;

  hyprland::HyprlandRuntime& CompositorRuntimeRegistry::hyprland() noexcept { return *m_hyprland; }

  const hyprland::HyprlandRuntime& CompositorRuntimeRegistry::hyprland() const noexcept { return *m_hyprland; }

  mango::MangoRuntime& CompositorRuntimeRegistry::mango() noexcept { return *m_mango; }

  const mango::MangoRuntime& CompositorRuntimeRegistry::mango() const noexcept { return *m_mango; }

  niri::NiriRuntime& CompositorRuntimeRegistry::niri() noexcept { return *m_niri; }

  const niri::NiriRuntime& CompositorRuntimeRegistry::niri() const noexcept { return *m_niri; }

  sway::SwayRuntime& CompositorRuntimeRegistry::sway() noexcept { return *m_sway; }

  const sway::SwayRuntime& CompositorRuntimeRegistry::sway() const noexcept { return *m_sway; }

  triad::TriadRuntime& CompositorRuntimeRegistry::triad() noexcept { return *m_triad; }

  const triad::TriadRuntime& CompositorRuntimeRegistry::triad() const noexcept { return *m_triad; }

  umbriel::UmbrielRuntime& CompositorRuntimeRegistry::umbriel() noexcept { return *m_umbriel; }

  const umbriel::UmbrielRuntime& CompositorRuntimeRegistry::umbriel() const noexcept { return *m_umbriel; }

} // namespace compositors
