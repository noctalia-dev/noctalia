#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class ClipboardService;
class Glyph;
class Image;
class Label;
struct wl_output;

class ClipboardWidget : public Widget {
public:
  struct Options {
    std::string glyph = "clipboard";
    std::string customImage;
    bool customImageColorize = false;
    bool showLabel = false;
    int maxWidth = 200;
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool peek = false;
    int peekDelayMs = kBarRevealDelayMs;
  };

  ClipboardWidget(const ClipboardService* clipboard, wl_output* output, Options options);

  void create() override;

private:
  void doUpdate(Renderer& renderer) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void syncLabel();

  const ClipboardService* m_clipboard = nullptr;
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  bool m_showLabel = false;
  float m_maxWidth = 200.0F;
  GlyphPosition m_glyphPosition = GlyphPosition::Before;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  std::string m_labelText;
  std::uint64_t m_labelSelectionSerial = 0;
};
