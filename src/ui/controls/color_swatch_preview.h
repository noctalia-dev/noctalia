#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

class Box;

struct ColorSwatchPreview {
  std::optional<ColorSpec> surface;
  std::vector<ColorSpec> swatches;

  [[nodiscard]] bool empty() const noexcept { return swatches.empty(); }
};

class ColorSwatchPreviewStrip final : public Flex {
public:
  static constexpr std::size_t kMaxSwatches = 4;

  ColorSwatchPreviewStrip();

  void setPreview(const ColorSwatchPreview& preview);
  void setMetricsFromFontSize(float fontSize);

  [[nodiscard]] float preferredWidth() const noexcept;
  [[nodiscard]] float preferredHeight() const noexcept { return m_height; }

private:
  void syncGeometry();
  void positionSwatches(float height);

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  std::array<Box*, kMaxSwatches> m_swatches{};
  std::size_t m_visibleSwatches = 0;
  float m_discSize = 10.0F;
  float m_gap = 5.0F;
  float m_paddingX = 4.0F;
  float m_paddingY = 4.0F;
  float m_height = 14.0F;
};
