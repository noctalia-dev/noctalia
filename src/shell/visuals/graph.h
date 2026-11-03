#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

#include <array>
#include <vector>

class GraphNode;
class Renderer;

// A line-graph control over up to three normalized [0..1] data series. Wraps
// the raw GraphNode so shell/widget code (and the UI-tree reconciler) stay
// behind the controls layer boundary, and owns the shader's scroll protocol:
// each series is uploaded with one extrapolated lead sample and count = n, so
// setScroll(progress) slides smoothly toward the next sample. Scroll 0 (the
// default) ends exactly on the last real sample — right for static data.
class Graph : public Node {
public:
  Graph();

  // Raw series data; pass an empty vector to clear a series.
  void setValues(std::vector<float> values) { setSeries(0, std::move(values)); }
  void setValues2(std::vector<float> values) { setSeries(1, std::move(values)); }
  void setValues3(std::vector<float> values) { setSeries(2, std::move(values)); }
  void setSeries(std::size_t index, std::vector<float> values);

  void setColor(const ColorSpec& color) { setSeriesColor(0, color); }
  void setColor2(const ColorSpec& color) { setSeriesColor(1, color); }
  void setColor3(const ColorSpec& color) { setSeriesColor(2, color); }
  // Explicit fixed color.
  void setColor(const Color& color) { setSeriesColor(0, fixedColorSpec(color)); }
  void setColor2(const Color& color) { setSeriesColor(1, fixedColorSpec(color)); }
  void setColor3(const Color& color) { setSeriesColor(2, fixedColorSpec(color)); }
  void setSeriesColor(std::size_t index, const ColorSpec& color);

  void setLineWidth(float width);
  void setFillOpacity(float opacity);

  // Inter-sample scroll progress [0..1] applied to every active series; call
  // per frame (e.g. from onFrameTick) for smooth scrolling between samples.
  void setScroll(float progress);

  // Uploads pending series data. Layout flushes automatically; call this when
  // feeding data outside a layout pass (update/frame-tick contexts).
  void sync(Renderer& renderer);

  void setSize(float width, float height) override;

private:
  void doLayout(Renderer& renderer) override;
  void applyPalette();

  GraphNode* m_node = nullptr;
  std::array<std::vector<float>, 3> m_series;
  std::array<ColorSpec, 3> m_colors = {
      colorSpecFromRole(ColorRole::Primary),
      colorSpecFromRole(ColorRole::Secondary),
      colorSpecFromRole(ColorRole::Tertiary),
  };
  float m_lineWidth = 1.5F;
  float m_fillOpacity = 0.2F;
  bool m_dataDirty = false;
  Signal<>::ScopedConnection m_paletteConn;
};
