#pragma once

#include "render/scene/node.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct KaraokeRange {
  std::size_t startByte = 0;
  std::size_t endByte = 0;

  bool operator==(const KaraokeRange&) const = default;
};

// A single shaped line with a progressive color fill over logical byte ranges.
class KaraokeLabel : public Node {
public:
  KaraokeLabel();

  Label& label() noexcept { return *m_label; }
  const Label& label() const noexcept { return *m_label; }
  void setRanges(std::vector<KaraokeRange> ranges);
  void setHighlightColor(const ColorSpec& color);
  void setProgress(std::size_t completedSegments, float fraction);
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

  struct HighlightClip {
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
  void rebuildHighlights(Renderer& renderer);
  void applyProgress();

  Label* m_label = nullptr;
  std::vector<KaraokeRange> m_ranges;
  std::vector<HighlightClip> m_clips;
  std::optional<ShapeKey> m_shapeKey;
  ColorSpec m_highlightColor = colorSpecFromRole(ColorRole::Primary);
  std::size_t m_completedSegments = 0;
  float m_fraction = 0.0F;
};
