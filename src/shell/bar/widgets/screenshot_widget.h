#pragma once

#include "capture/screenshot_service.h"
#include "config/config_types.h"
#include "shell/bar/widget.h"

#include <memory>
#include <string>

class CompositorPlatform;
class ConfigService;
class ContextMenuPopup;
class Glyph;
class InputArea;
class RenderContext;
struct wl_output;

class ScreenshotWidget : public Widget {
public:
  ScreenshotWidget(
      wl_output* output, std::string barGlyphId, ScreenshotService& screenshots, ConfigService& configService,
      CompositorPlatform& platform, RenderContext& renderContext, const ShellConfig::ShadowConfig& shadow,
      std::string barPosition = "top"
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
  Glyph* m_glyph = nullptr;
  InputArea* m_hitArea = nullptr;
  std::unique_ptr<ContextMenuPopup> m_menuPopup;
};
