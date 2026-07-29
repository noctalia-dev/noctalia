#pragma once

#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/flex.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class Box;
class InputArea;
class Glyph;
class Label;
class ColorSwatchPreviewStrip;
class RectNode;

class Select : public Flex {
public:
  Select();
  ~Select() override;

  void setOptions(std::vector<std::string> options);
  void setSelectedIndex(std::size_t index);
  void setSelectedIndexSilently(std::size_t index);
  void clearSelection();
  void setEnabled(bool enabled);
  void setSurfaceOpacity(float opacity);
  void setPlaceholder(std::string_view placeholder);
  void setFontSize(float size);
  void setControlHeight(float height);
  void setHorizontalPadding(float padding);
  void setGlyphSize(float size);
  void setOptionIndicators(std::vector<ColorSpec> colors);
  void setColorSwatchPreviews(std::vector<ColorSwatchPreview> previews);
  void setNotifyOnReselect(bool enabled);
  void setOnSelectionChanged(std::function<void(std::size_t, std::string_view)> callback);

  [[nodiscard]] std::size_t selectedIndex() const noexcept { return m_selectedIndex; }
  [[nodiscard]] std::string_view selectedText() const noexcept;
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool open() const noexcept { return m_open; }

private:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void syncTriggerText();
  void applyVisualState();
  void animateCaret(bool open);
  void setSelectedIndexInternal(std::size_t index, bool notify);
  void handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed);
  void toggleOpen();
  void closeMenu();
  void openPopupDropdown();

  RectNode* m_triggerBackground = nullptr;
  Box* m_triggerIndicator = nullptr;
  ColorSwatchPreviewStrip* m_triggerPreview = nullptr;
  Label* m_triggerLabel = nullptr;
  Glyph* m_triggerGlyph = nullptr;
  InputArea* m_triggerArea = nullptr;

  std::vector<std::string> m_options;
  std::size_t m_selectedIndex = npos;
  std::string m_placeholder;
  float m_surfaceOpacity = 1.0f;
  bool m_enabled = true;
  bool m_open = false;
  float m_caretProgress = 0.0f;
  std::uint32_t m_caretAnimId = 0;
  float m_fixedWidth = 0.0f;
  float m_fontSize = Style::fontSizeBody;
  float m_controlHeight = Style::controlHeight;
  float m_horizontalPadding = Style::spaceMd;
  float m_glyphSize = 14.0f;
  std::vector<ColorSpec> m_indicatorColors;
  std::vector<ColorSwatchPreview> m_optionSwatchPreviews;
  bool m_notifyOnReselect = false;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_inputBordersConn;

  std::function<void(std::size_t, std::string_view)> m_onSelectionChanged;
};
