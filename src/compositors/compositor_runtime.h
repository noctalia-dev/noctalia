#pragma once

#include <memory>

namespace compositors {

  namespace hyprland {
    class HyprlandRuntime;
  } // namespace hyprland

  namespace mango {
    class MangoRuntime;
  } // namespace mango

  namespace niri {
    class NiriRuntime;
  } // namespace niri

  namespace sway {
    class SwayRuntime;
  } // namespace sway

  namespace umbriel {
    class UmbrielRuntime;
  } // namespace umbriel

  class CompositorRuntimeRegistry {
  public:
    CompositorRuntimeRegistry();
    ~CompositorRuntimeRegistry();

    CompositorRuntimeRegistry(const CompositorRuntimeRegistry&) = delete;
    CompositorRuntimeRegistry& operator=(const CompositorRuntimeRegistry&) = delete;

    hyprland::HyprlandRuntime& hyprland() noexcept;
    const hyprland::HyprlandRuntime& hyprland() const noexcept;
    mango::MangoRuntime& mango() noexcept;
    const mango::MangoRuntime& mango() const noexcept;
    niri::NiriRuntime& niri() noexcept;
    const niri::NiriRuntime& niri() const noexcept;
    sway::SwayRuntime& sway() noexcept;
    const sway::SwayRuntime& sway() const noexcept;
    umbriel::UmbrielRuntime& umbriel() noexcept;
    const umbriel::UmbrielRuntime& umbriel() const noexcept;

  private:
    std::unique_ptr<hyprland::HyprlandRuntime> m_hyprland;
    std::unique_ptr<mango::MangoRuntime> m_mango;
    std::unique_ptr<niri::NiriRuntime> m_niri;
    std::unique_ptr<sway::SwayRuntime> m_sway;
    std::unique_ptr<umbriel::UmbrielRuntime> m_umbriel;
  };

} // namespace compositors
