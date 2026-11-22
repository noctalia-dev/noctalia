#pragma once

#include "render/scene/node.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One fill segment: a [startByte, endByte) range of the label text. Both ends must fall on UTF-8
// character boundaries; segments that do not are skipped.
struct FillRange {
  std::size_t startByte = 0;
  std::size_t endByte = 0;

  bool operator==(const FillRange&) const = default;
};

// A single shaped line with a progressive color fill over logical byte ranges. Segments before the
// progress point are fully filled, the current one partially (left to right, right to left for RTL
// runs). Changing only the progress moves clip nodes and never reshapes the text.
class FillLabel : public Node {
public:
  FillLabel();

  Label& label() noexcept { return *m_label; }
  const Label& label() const noexcept { return *m_label; }
  void setFillRanges(std::vector<FillRange> ranges);
  void setFillColor(const ColorSpec& color);
  // `completedSegments` segments are fully filled; the next one is filled by `fraction` (0..1).
  void setFillProgress(std::size_t completedSegments, float fraction);
  void measure(Renderer& renderer);

private:
  struct ShapeKey {
    std::string text;
    TextFont font;
    float fontSize = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float maxWidth = 0.0F;
    float textOriginX = 0.0F;
    float baseline = 0.0F;
    float renderScale = 0.0F;
    std::uint64_t fontGeneration = 0;
    TextAlign align = TextAlign::Start;
    TextEllipsize ellipsize = TextEllipsize::End;
    LabelBaselineMode baselineMode = LabelBaselineMode::Text;

    bool operator==(const ShapeKey&) const = default;
  };

  struct FillClip {
    Node* clip = nullptr;
    Label* label = nullptr;
    std::size_t segment = 0;
    float left = 0.0F;
    float right = 0.0F;
    bool rtl = false;
  };

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void syncGeometry(Renderer& renderer);
  void rebuildFills(Renderer& renderer);
  void applyProgress();

  Label* m_label = nullptr;
  std::vector<FillRange> m_ranges;
  std::vector<FillClip> m_clips;
  std::optional<ShapeKey> m_shapeKey;
  ColorSpec m_fillColor = colorSpecFromRole(ColorRole::Primary);
  std::size_t m_completedSegments = 0;
  float m_fraction = 0.0F;
};
