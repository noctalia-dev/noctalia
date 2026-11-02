#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"

#include <algorithm>
#include <vector>

class AudioSpectrumNode : public Node {
public:
  AudioSpectrumNode() : Node(NodeType::AudioSpectrum) {}

  const AudioSpectrumStyle& style() const noexcept { return m_style; }
  const std::vector<float>& values() const noexcept { return m_values; }

  void setStyle(const AudioSpectrumStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

  bool setSpectrumValues(const std::vector<float>& values) {
    if (m_values.size() == values.size() && std::equal(values.begin(), values.end(), m_values.begin())) {
      return false;
    }
    m_values = values;
    markPaintDirty();
    return true;
  }

private:
  AudioSpectrumStyle m_style;
  std::vector<float> m_values;
};
