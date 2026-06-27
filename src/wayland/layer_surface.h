#pragma once

#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

struct wl_output;
struct zwlr_layer_surface_v1;

enum class LayerShellLayer : std::uint32_t {
  Background = 0,
  Bottom = 1,
  Top = 2,
  Overlay = 3,
};

enum class LayerShellKeyboard : std::uint32_t {
  None = 0,
  Exclusive = 1,
  OnDemand = 2,
};

[[nodiscard]] LayerShellLayer layerShellLayerFromConfig(std::string_view layer);

namespace LayerShellAnchor {
  inline constexpr std::uint32_t Top = 1;
  inline constexpr std::uint32_t Bottom = 2;
  inline constexpr std::uint32_t Left = 4;
  inline constexpr std::uint32_t Right = 8;
} // namespace LayerShellAnchor

struct LayerSurfaceConfig {
  std::string nameSpace = "noctalia";
  LayerShellLayer layer = LayerShellLayer::Top;
  std::uint32_t anchor = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int32_t exclusiveZone = 0;
  std::int32_t marginTop = 0;
  std::int32_t marginRight = 0;
  std::int32_t marginBottom = 0;
  std::int32_t marginLeft = 0;
  LayerShellKeyboard keyboard = LayerShellKeyboard::None;
  std::uint32_t defaultWidth = 1920;
  std::uint32_t defaultHeight = 0;
  bool prewarmBlur = false;
};

class LayerSurface : public Surface {
public:
  LayerSurface(WaylandConnection& connection, LayerSurfaceConfig config);
  ~LayerSurface() override;

  bool initialize() override;
  bool initialize(wl_output* output);
  [[nodiscard]] zwlr_layer_surface_v1* layerSurface() const noexcept { return m_layerSurface; }

  // Requests a new surface size from the compositor. The compositor will
  // respond with a configure event, which triggers the configure callback.
  void requestSize(std::uint32_t width, std::uint32_t height);
  void setLayer(LayerShellLayer layer);
  void setMargins(std::int32_t top, std::int32_t right, std::int32_t bottom, std::int32_t left);
  void setExclusiveZone(std::int32_t exclusiveZone);
  void setClickThrough(bool clickThrough);
  void setKeyboardInteractivity(LayerShellKeyboard mode);
  void setClosedCallback(std::function<void()> callback);
  [[nodiscard]] LayerShellKeyboard keyboardInteractivity() const noexcept { return m_config.keyboard; }
  [[nodiscard]] std::uint32_t anchor() const noexcept { return m_config.anchor; }
  [[nodiscard]] std::int32_t marginTop() const noexcept { return m_config.marginTop; }
  [[nodiscard]] std::int32_t marginRight() const noexcept { return m_config.marginRight; }
  [[nodiscard]] std::int32_t marginBottom() const noexcept { return m_config.marginBottom; }
  [[nodiscard]] std::int32_t marginLeft() const noexcept { return m_config.marginLeft; }

  static void handleConfigure(
      void* data, zwlr_layer_surface_v1* layerSurface, std::uint32_t serial, std::uint32_t width, std::uint32_t height
  );
  static void handleClosed(void* data, zwlr_layer_surface_v1* layerSurface);

private:
  void applyInputRegion();

  LayerSurfaceConfig m_config;
  zwlr_layer_surface_v1* m_layerSurface = nullptr;
  std::function<void()> m_closedCallback;
  bool m_clickThrough = false;
};
