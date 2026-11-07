#pragma once

#include "wayland/surface.h"

#include <cstdint>

struct wl_output;
struct wl_subsurface;
struct wl_surface;

enum class SubsurfaceStacking {
  AboveParent,
  BelowParent,
};

struct SubsurfaceConfig {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  SubsurfaceStacking stacking = SubsurfaceStacking::AboveParent;
  bool desynchronized = true;
};

class Subsurface : public Surface {
public:
  Subsurface(WaylandConnection& connection, SubsurfaceConfig config);
  ~Subsurface() override;

  bool initialize() override;
  bool initialize(wl_surface* parentSurface, wl_output* output);

  void setPosition(std::int32_t x, std::int32_t y);
  void setStacking(SubsurfaceStacking stacking);

private:
  void applyStacking();

  SubsurfaceConfig m_config;
  wl_surface* m_parentSurface = nullptr;
  wl_output* m_output = nullptr;
  wl_subsurface* m_subsurface = nullptr;
};
