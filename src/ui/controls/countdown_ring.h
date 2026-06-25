#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/signal.h"

class CountdownRingNode;
class Label;

class CountdownRing : public Node {
public:
  CountdownRing();

  void setRingSize(float size);
  void setThickness(float thickness);
  void setFontSize(float size);
  void setProgress(float progress);
  void setSeconds(int seconds);
  void setColor(const ColorSpec& color);
  void setColor(const Color& color);

  [[nodiscard]] float ringSize() const noexcept { return m_ringSize; }

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;

private:
  void applyPalette();
  void syncGeometry();

  CountdownRingNode* m_ringNode = nullptr;
  Label* m_secondsLabel = nullptr;
  ColorSpec m_color = colorSpecFromRole(ColorRole::Primary);
  Signal<>::ScopedConnection m_paletteConn;
  float m_ringSize = 64.0f;
  float m_thickness = 6.0f;
  float m_fontSize = 22.0f;
};
