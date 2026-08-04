#include "ui/controls/graph.h"

#include "render/core/renderer.h"
#include "render/scene/graph_node.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace {

  // The shader scrolls toward one sample past the series end, so each upload
  // carries an extrapolated lead sample; count stays at the real sample count.
  std::vector<float> withExtrapolatedLead(const std::vector<float>& values) {
    std::vector<float> out = values;
    if (values.size() >= 2) {
      const float last = values[values.size() - 1];
      const float prev = values[values.size() - 2];
      out.push_back(std::clamp(last + (last - prev) * 0.5F, 0.0F, 1.0F));
    } else if (!values.empty()) {
      out.push_back(values.back());
    }
    return out;
  }

} // namespace

Graph::Graph() {
  auto node = std::make_unique<GraphNode>();
  node->setLineWidth(m_lineWidth);
  node->setGraphFillOpacity(m_fillOpacity);
  m_node = static_cast<GraphNode*>(addChild(std::move(node)));
  m_paletteConn = paletteChanged().connect([this]() { applyPalette(); });
  applyPalette();
}

void Graph::setSeries(std::size_t index, std::vector<float> values) {
  if (index >= m_series.size() || values == m_series[index]) {
    return;
  }
  m_series[index] = std::move(values);
  m_dataDirty = true;
  markPaintDirty();
}

void Graph::setSeriesColor(std::size_t index, const ColorSpec& color) {
  if (index >= m_colors.size()) {
    return;
  }
  m_colors[index] = color;
  applyPalette();
}

void Graph::setLineWidth(float width) {
  m_lineWidth = width;
  if (m_node != nullptr) {
    m_node->setLineWidth(width);
  }
}

void Graph::setFillOpacity(float opacity) {
  m_fillOpacity = opacity;
  if (m_node != nullptr) {
    m_node->setGraphFillOpacity(opacity);
  }
}

void Graph::setScroll(float progress) {
  if (m_node == nullptr) {
    return;
  }
  const float clamped = std::clamp(progress, 0.0F, 1.0F);
  m_node->setScroll1(clamped);
  m_node->setScroll2(clamped);
  m_node->setScroll3(clamped);
}

void Graph::setSize(float width, float height) {
  Node::setSize(width, height);
  if (m_node != nullptr) {
    m_node->setPosition(0.0F, 0.0F);
    m_node->setSize(width, height);
  }
}

void Graph::applyPalette() {
  if (m_node == nullptr) {
    return;
  }
  m_node->setLineColor1(resolveColorSpec(m_colors[0]));
  m_node->setLineColor2(resolveColorSpec(m_colors[1]));
  m_node->setLineColor3(resolveColorSpec(m_colors[2]));
}

void Graph::doLayout(Renderer& renderer) {
  if (m_node == nullptr) {
    return;
  }
  m_node->setPosition(0.0F, 0.0F);
  m_node->setSize(width(), height());
  sync(renderer);
}

void Graph::sync(Renderer& renderer) {
  if (m_node == nullptr || !m_dataDirty) {
    return;
  }
  m_dataDirty = false;

  if (m_series[0].empty() && m_series[1].empty() && m_series[2].empty()) {
    m_node->setCount1(0.0F);
    m_node->setCount2(0.0F);
    m_node->setCount3(0.0F);
    return;
  }

  const std::vector<float> lead1 = withExtrapolatedLead(m_series[0]);
  const std::vector<float> lead2 = withExtrapolatedLead(m_series[1]);
  const std::vector<float> lead3 = withExtrapolatedLead(m_series[2]);
  m_node->setData(
      renderer.textureManager(), lead1.empty() ? nullptr : lead1.data(), static_cast<int>(lead1.size()),
      lead2.empty() ? nullptr : lead2.data(), static_cast<int>(lead2.size()), lead3.empty() ? nullptr : lead3.data(),
      static_cast<int>(lead3.size())
  );
  m_node->setCount1(static_cast<float>(m_series[0].size()));
  m_node->setCount2(static_cast<float>(m_series[1].size()));
  m_node->setCount3(static_cast<float>(m_series[2].size()));
}
