#pragma once

#include "config/config_types.h"
#include "shell/frame/frame_geometry.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class ConfigService;
class RenderContext;
class WaylandConnection;

// Draws a solid inset border around each output with a rounded hole in the middle that the
// desktop shows through.
//
// The visual is a single full-output click-through surface rather than one strip per edge:
// the rounded inner corners straddle two edges, so per-edge surfaces each rounding their own
// corners meet at a hard angle instead of a continuous arc. Space is reserved by separate
// zero-visual strips, since a layer surface can only reserve the one edge it is anchored to.
//
// The edge carrying a space-reserving bar is inset by the bar's own thickness, not by
// `thickness` plus it — the bar *is* the frame on that edge, and the hole supplies its inner
// corners so the two read as one shape.
class Frame {
public:
  Frame() = default;
  ~Frame() = default;

  Frame(const Frame&) = delete;
  Frame& operator=(const Frame&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onOutputChange();
  void onConfigReload();
  void requestRedraw();
  // Auto-hide slides the bar out of a band it still reserves, leaving it empty. Repaint so the
  // frame covers that band instead of exposing the wallpaper behind it.
  void onBarVisibilityChanged();
  // Bar runtime state. Visibility and reservation answer different questions and must stay
  // separate: the frame cedes the bar's edge while the bar is on screen, and what it paints
  // once the bar leaves depends on whether the band stayed reserved.
  void setBarStateProviders(std::function<bool()> visible, std::function<bool()> reserves);

private:
  // Reserved insets of the hole from each output edge, in logical px.
  using Insets = frame_geometry::Insets;

  struct OutputInstance {
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> visual;
    std::unique_ptr<Node> sceneRoot;
    // One per edge, in ScreenEdge order; null on the edge a bar already reserves.
    std::unique_ptr<LayerSurface> reservations[4];
    // Output size to build against before the compositor's first configure lands, which is
    // when the render target still reports 0×0.
    std::uint32_t fallbackWidth = 0;
    std::uint32_t fallbackHeight = 0;
    std::uint32_t builtWidth = 0;
    std::uint32_t builtHeight = 0;
  };

  [[nodiscard]] const ShellConfig::FrameConfig& frameConfig() const;
  // Hole insets for an output: the bar's thickness on the bar's edge, `thickness` elsewhere.
  [[nodiscard]] Insets holeInsets() const;
  [[nodiscard]] bool barVisible() const;
  [[nodiscard]] bool barReserves() const;
  [[nodiscard]] float fillOpacity() const;

  void ensureSurfaces();
  void destroySurfaces();
  void buildFrameScene(OutputInstance& instance, std::uint32_t width, std::uint32_t height);

  std::function<bool()> m_barVisibleProvider;
  std::function<bool()> m_barReservesProvider;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  bool m_lastEnabled = false;
  ShellConfig::FrameConfig m_lastConfig;
  Insets m_lastInsets;
  std::vector<std::unique_ptr<OutputInstance>> m_instances;
};
