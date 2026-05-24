#pragma once

#include <memory>

namespace compositors {

  namespace hyprland {
    class HyprlandRuntime;
  } // namespace hyprland

  namespace niri {
    class NiriRuntime;
  } // namespace niri

  namespace sway {
    class SwayRuntime;
  } // namespace sway

  namespace triad {
    class TriadRuntime;
  } // namespace triad

  class CompositorRuntimeRegistry {
  public:
    CompositorRuntimeRegistry();
    ~CompositorRuntimeRegistry();

    CompositorRuntimeRegistry(const CompositorRuntimeRegistry&) = delete;
    CompositorRuntimeRegistry& operator=(const CompositorRuntimeRegistry&) = delete;

    [[nodiscard]] hyprland::HyprlandRuntime& hyprland() noexcept;
    [[nodiscard]] const hyprland::HyprlandRuntime& hyprland() const noexcept;
    [[nodiscard]] niri::NiriRuntime& niri() noexcept;
    [[nodiscard]] const niri::NiriRuntime& niri() const noexcept;
    [[nodiscard]] sway::SwayRuntime& sway() noexcept;
    [[nodiscard]] const sway::SwayRuntime& sway() const noexcept;
    [[nodiscard]] triad::TriadRuntime& triad() noexcept;
    [[nodiscard]] const triad::TriadRuntime& triad() const noexcept;

  private:
    std::unique_ptr<hyprland::HyprlandRuntime> m_hyprland;
    std::unique_ptr<niri::NiriRuntime> m_niri;
    std::unique_ptr<sway::SwayRuntime> m_sway;
    std::unique_ptr<triad::TriadRuntime> m_triad;
  };

} // namespace compositors
