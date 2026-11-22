#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <string>
#include <vector>

class Glyph;
class Graph;
class Label;
class ProgressBar;
enum class DesktopSysmonDisplayMode : std::uint8_t { Graph, Gauge };

enum class DesktopSysmonGaugeLayout : std::uint8_t { Horizontal, Vertical };

class DesktopSysmon : public Node {
public:
  struct Options {
    std::string glyph = "cpu-usage";
    std::string glyph2;
    DesktopSysmonDisplayMode displayMode = DesktopSysmonDisplayMode::Graph;
    DesktopSysmonGaugeLayout gaugeLayout = DesktopSysmonGaugeLayout::Horizontal;
    ColorSpec lineColor = colorSpecFromRole(ColorRole::Primary);
    ColorSpec lineColor2 = colorSpecFromRole(ColorRole::Secondary);
    bool showLabel = true;
    float labelMinWidth = 0.0F;
    bool shadow = true;
  };
  explicit DesktopSysmon(const Options& options);
  void setAppearance(
      DesktopSysmonGaugeLayout layout, const ColorSpec& color, const ColorSpec& color2, float labelMinWidth, bool shadow
  );
  void setText(const std::string& text, const std::string& text2);
  void setGauge(double progress, const Color& color);
  void setHistory(Renderer& renderer, std::vector<float> primary, std::vector<float> secondary);
  void clearHistory();
  void setScroll(float progress);
  void setFontFamily(const std::string& family);
  void setContentScale(float scale);

private:
  void doLayout(Renderer& renderer) override;
  void layoutGraphMode(Renderer& renderer);
  void layoutGaugeMode(Renderer& renderer);
  void syncGaugeProgress();
  void applyShadow();
  std::string m_glyphName;
  bool m_hasSecond = false;
  DesktopSysmonDisplayMode m_displayMode;
  DesktopSysmonGaugeLayout m_gaugeLayout;
  ColorSpec m_lineColor;
  ColorSpec m_lineColor2;
  bool m_showLabel;
  float m_labelMinWidth;
  bool m_shadow;
  float m_contentScale = 1.0F;
  Glyph* m_glyph = nullptr;
  Glyph* m_glyph2 = nullptr;
  Label* m_label = nullptr;
  Label* m_label2 = nullptr;
  Graph* m_graph = nullptr;
  ProgressBar* m_gauge = nullptr;
  double m_progress = 0.0;
  Color m_valueColor{};
};
