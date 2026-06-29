#pragma once

#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "render/scene/text_node.h"
#include "ui/palette.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class Renderer;

enum class LabelBaselineMode : std::uint8_t {
  StableLogical,
  InkCentered,
  // Cap-band centering like StableLogical, but the box height comes from the
  // primary font's line extent (measureFont) instead of the per-string metrics.
  // Prevents fallback fonts for unusual Unicode characters from inflating the
  // label height — useful in lists with unpredictable content.
  StableFont,
  // Centers a cap-height band anchored at the glyph's ink top instead of the
  // baseline. For pictographic script fonts (e.g. bongocat poses) the ink top is
  // the fixed part of the art and lower ink moves per glyph: this keeps the art
  // vertically put (no bob, unlike InkCentered) and centered (cap-band-from-baseline
  // sits it too high). Degrades to cap-band centering for normal text.
  StableFontBox,
};

class Label : public InputArea {
public:
  Label();

  bool setText(std::string_view text);
  void setFontSize(float size);
  void setFontFamily(std::string family);
  void setColor(const ColorSpec& color);
  // Explicit fixed color.
  void setColor(const Color& color);
  void setMinWidth(float minWidth);
  void setMaxWidth(float maxWidth);
  void setMaxLines(int maxLines);
  void setFontWeight(FontWeight fontWeight);
  void setTextAlign(TextAlign align);
  // Which end of an overflowing single line gets the ellipsis (Start keeps the tail, e.g. file paths).
  void setEllipsize(TextEllipsize ellipsize);
  void setUseMarkup(bool markup);
  // StableLogical uses the resolved font line box; InkCentered centers the current glyph ink.
  void setBaselineMode(LabelBaselineMode mode);
  void setShadow(const Color& color, float offsetX, float offsetY);
  void clearShadow();
  // Single-line horizontal marquee when the line is wider than the laid-out width.
  // Constrain width with parent layout and/or setMaxWidth() — Flex ignores preset setSize().
  // Requires an AnimationManager on the scene (via setAnimationManager).
  void setAutoScroll(bool enabled);
  void setAutoScrollSpeed(float pixelsPerSecond);
  // When true (with auto-scroll), marquee runs only while the pointer is over the label.
  void setAutoScrollOnlyWhenHovered(bool enabled);
  [[nodiscard]] bool autoScroll() const noexcept { return m_autoScroll; }
  [[nodiscard]] float autoScrollSpeed() const noexcept { return m_scrollSpeedPxPerSec; }
  [[nodiscard]] bool autoScrollOnlyWhenHovered() const noexcept { return m_autoScrollHoverOnly; }

  [[nodiscard]] const std::string& text() const noexcept;
  [[nodiscard]] float fontSize() const noexcept;
  [[nodiscard]] const Color& color() const noexcept;
  [[nodiscard]] float maxWidth() const noexcept;
  [[nodiscard]] FontWeight fontWeight() const noexcept;
  [[nodiscard]] TextAlign textAlign() const noexcept;
  [[nodiscard]] TextEllipsize ellipsize() const noexcept;
  [[nodiscard]] LabelBaselineMode baselineMode() const noexcept { return m_baselineMode; }
  [[nodiscard]] float baselineOffset() const noexcept { return m_baselineOffset; }

  void measure(Renderer& renderer);

private:
  // Labels opt out of hit-testing by default; a tooltip needs hits to reach the label.
  void onTooltipChanged() override { syncHoverInteraction(); }
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void applyPalette();
  LayoutSize measureWithConstraints(Renderer& renderer, const LayoutConstraints& constraints, bool fromArrange = false);
  void syncTextNodeConstraints();
  void restartScrollIfNeeded();
  void stopMarqueeAnimation();
  void stopSnapAnimation();
  void stopScrollAnimations();
  void startMarqueeLoop();
  void startSnapToZero();
  void applyScrollPosition();
  void syncHoverInteraction();

  // Guard token for deferred callbacks that run on the next main-loop tick.
  // Callbacks capture a weak_ptr so they can detect destruction without
  // relying on a raw this pointer staying valid.
  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  TextNode* m_textNode = nullptr;
  float m_minWidth = 0.0f;
  float m_baselineOffset = 0.0f;
  ColorSpec m_color = colorSpecFromRole(ColorRole::OnSurface);
  Signal<>::ScopedConnection m_paletteConn;

  // User-visible text (wire text may duplicate for seamless marquee).
  std::string m_plainText;

  // Memoized measure() inputs — lets repeated layout passes with identical
  // text skip the Pango/fontconfig path entirely.
  std::string m_cachedText;
  float m_cachedFontSize = 0.0f;
  float m_cachedMaxWidth = 0.0f;
  float m_cachedMinWidth = 0.0f;
  float m_cachedConstraintMinWidth = 0.0f;
  float m_cachedConstraintMaxWidth = 0.0f;
  float m_cachedRenderScale = 0.0f;
  std::uint64_t m_cachedTextMetricsGeneration = 0;
  int m_cachedMaxLines = 0;
  TextAlign m_cachedTextAlign = TextAlign::Start;
  LabelBaselineMode m_cachedBaselineMode = LabelBaselineMode::StableLogical;
  FontWeight m_cachedFontWeight = FontWeight::Normal;
  bool m_cachedAutoScroll = false;
  bool m_cachedHasConstraintMaxWidth = false;
  bool m_measureCached = false;
  LabelBaselineMode m_baselineMode = LabelBaselineMode::StableLogical;

  float m_userMaxWidth = 0.0f;
  int m_userMaxLines = 0;
  bool m_autoScroll = false;
  bool m_autoScrollHoverOnly = false;
  float m_scrollSpeedPxPerSec = 48.0f;
  float m_scrollOffset = 0.0f;
  float m_fullTextWidth = 0.0f;
  float m_marqueeLoopPeriod = 0.0f;
  float m_textBaseX = 0.0f;
  std::uint32_t m_marqueeAnimId = 0;
  std::uint32_t m_snapAnimId = 0;

  // Last-applied marquee inputs. restartScrollIfNeeded() resets the scroll
  // offset to 0 and re-arms the animation, so we must avoid calling it on
  // every measure pass — Flex layout calls Label::measureWithConstraints()
  // multiple times per layout (measure → arrange → next pass) with slightly
  // different constraints, each of which is a measure-cache miss.
  bool m_marqueeStateValid = false;
  bool m_marqueeStateAutoScroll = false;
  bool m_marqueeStateHoverOnly = false;
  bool m_marqueeStateHovered = false;
  float m_marqueeStateWidth = 0.0f;
  float m_marqueeStateFullTextWidth = 0.0f;
  float m_marqueeStateLoopPeriod = 0.0f;
  float m_marqueeStateSpeed = 0.0f;
};
