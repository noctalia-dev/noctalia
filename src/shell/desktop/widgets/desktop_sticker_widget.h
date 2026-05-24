#pragma once

#include "core/timer_manager.h"
#include "render/core/texture_manager.h"
#include "shell/desktop/desktop_widget.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

class Image;

class DesktopStickerWidget : public DesktopWidget {
public:
  DesktopStickerWidget(std::string imagePath, float opacity);
  ~DesktopStickerWidget() override;

  void create() override;
  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  struct AnimatedFrame {
    TextureHandle handle{};
    std::chrono::milliseconds duration{0};
  };

  void doLayout(Renderer& renderer) override;
  bool tryLoadAnimated(Renderer& renderer);
  void scheduleNextFrame();
  void onFrameTimer();
  void unloadFrames();

  std::string m_imagePath;
  float m_opacity = 1.0f;
  Image* m_image = nullptr;
  bool m_loaded = false;

  std::vector<AnimatedFrame> m_frames;
  std::size_t m_currentFrame = 0;
  Timer m_frameTimer;
  Renderer* m_renderer = nullptr;
};
