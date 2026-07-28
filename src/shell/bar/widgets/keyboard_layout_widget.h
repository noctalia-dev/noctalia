#pragma once

#include "core/timer_manager.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <string>
#include <unordered_map>

class Glyph;
class Image;
class Label;
class Renderer;
class CompositorPlatform;

class KeyboardLayoutWidget : public Widget {
public:
  enum class DisplayMode : std::uint8_t { Short = 0, Full = 1 };

  KeyboardLayoutWidget(
      CompositorPlatform& platform, DisplayMode displayMode, bool showIcon, bool showLabel, bool hideWhenSingleLayout,
      std::unordered_map<std::string, std::string> customLabels = {}, std::string glyph = "keyboard",
      WidgetCustomImage customImage = {}
  );
  static DisplayMode parseDisplayMode(const std::string& value);
  static std::string formatLayoutLabel(const std::string& layoutName, DisplayMode displayMode);
  static std::string resolveLayoutLabel(
      const std::string& layoutName, DisplayMode displayMode,
      const std::unordered_map<std::string, std::string>& customLabels
  );

  void create() override;

private:
  void onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);
  [[nodiscard]] std::string resolvedLayoutName() const;
  void armRefreshTick();
  void scheduleRefreshBurst();

  CompositorPlatform& m_platform;
  DisplayMode m_displayMode = DisplayMode::Short;
  bool m_showIcon = true;
  bool m_showLabel = true;
  bool m_hideWhenSingleLayout = false;
  std::unordered_map<std::string, std::string> m_customLabels;
  std::string m_glyphName = "keyboard";
  WidgetCustomImage m_customImage;

  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;

  std::string m_lastLayoutName;
  std::string m_lastLabel;
  std::string m_pendingLayoutName;
  int m_refreshAttemptsRemaining = 0;
  Timer m_refreshTimer;
  bool m_isVertical = false;
  bool m_lastVertical = false;
};
