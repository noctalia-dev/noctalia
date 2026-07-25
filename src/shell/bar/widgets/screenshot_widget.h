#pragma once

#include "capture/screenshot_service.h"
#include "config/config_types.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <memory>
#include <string>

class CompositorPlatform;
class ConfigService;
class ContextMenuPopup;
class Glyph;
class Image;
class InputArea;
class RenderContext;
struct wl_output;

class ScreenshotWidget : public Widget {
public:
  enum class PrimaryClick : std::uint8_t {
    Region,
    Fullscreen,
  };

  struct Options {
    std::string glyph = "screenshot";
    std::string customImage;
    bool customImageColorize = false;
    PrimaryClick primaryClick = PrimaryClick::Region;

    bool operator==(const Options&) const = default;
  };

  ScreenshotWidget(
      wl_output* output, ScreenshotService& screenshots, ConfigService& configService, CompositorPlatform& platform,
      RenderContext& renderContext, std::string barPosition, Options options
  );
  ~ScreenshotWidget() override;

  void create() override;
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event) override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void openCaptureMenu();
  void runPrimaryClickAction();
  [[nodiscard]] ScreenshotService::OutputOptions outputOptions() const;
  [[nodiscard]] bool primaryClickIsFullscreen() const;

  std::string m_barGlyphId;
  wl_output* m_output = nullptr;
  ScreenshotService& m_screenshots;
  ConfigService& m_configService;
  CompositorPlatform& m_platform;
  RenderContext& m_renderContext;
  ShellConfig::ShadowConfig m_shadowConfig;
  std::string m_barPosition;
  WidgetCustomImage m_customImage;
  PrimaryClick m_primaryClick = PrimaryClick::Region;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  InputArea* m_hitArea = nullptr;
  std::unique_ptr<ContextMenuPopup> m_menuPopup;
};
