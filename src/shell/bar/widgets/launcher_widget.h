#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class Glyph;
class Image;
struct wl_output;

class LauncherWidget : public Widget {
public:
  struct Options {
    std::string glyph = "search";
    std::string customImage;
    bool customImageColorize = false;

    bool operator==(const Options&) const = default;
  };

  LauncherWidget(wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
};
