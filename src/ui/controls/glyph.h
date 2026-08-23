#pragma once

#include "render/core/color.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <optional>
#include <string_view>

class GlyphNode;
class Renderer;

class Glyph : public Node {
public:
  Glyph();

  bool setGlyph(std::string_view name);
  bool setCodepoint(char32_t codepoint);
  void setGlyphSize(float size);
  void setColor(const ColorSpec& color);
  // Explicit fixed color.
  void setColor(const Color& color);
  void setShadow(const ColorSpec& color, float offsetX, float offsetY);
  void setShadow(const Color& color, float offsetX, float offsetY);
  void clearShadow();

  void measure(Renderer& renderer);

  [[nodiscard]] float baselineOffset() const noexcept { return m_baselineOffset; }

private:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void applyPalette();
  LayoutSize measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints);

  GlyphNode* m_glyphNode = nullptr;
  float m_baselineOffset = 0.0F;
  float m_logicalFontSize = 0.0F;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  Signal<>::ScopedConnection m_paletteConn;
  std::optional<ColorSpec> m_shadowColor;
  float m_shadowOffsetX = 0.0F;
  float m_shadowOffsetY = 0.0F;

  // Memoized measure() inputs — lets repeated layout passes with identical
  // glyph + size skip the Pango/fontconfig path entirely.
  char32_t m_cachedCodepoint = 0;
  float m_cachedFontSize = 0.0F;
  float m_cachedLogicalFontSize = 0.0F;
  float m_cachedConstraintMaxWidth = 0.0F;
  float m_cachedConstraintMaxHeight = 0.0F;
  float m_cachedRenderScale = 0.0F;
  bool m_cachedHasConstraintMaxWidth = false;
  bool m_cachedHasConstraintMaxHeight = false;
  bool m_measureCached = false;
};
