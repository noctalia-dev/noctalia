#pragma once

#include "config/config_types.h"
#include "shell/desktop/desktop_widget.h"

#include <array>
#include <unordered_map>

class Box;
class Glyph;

class DesktopLoginBoxWidget : public DesktopWidget {
public:
  void create() override;
  void layout(Renderer& renderer) override;
  void setScreenMetrics(float screenWidth, float screenHeight, float panelCenterY) noexcept {
    m_screenWidth = screenWidth;
    m_screenHeight = screenHeight;
    m_panelCenterY = panelCenterY;
  }
  void setSettings(const std::unordered_map<std::string, WidgetSettingValue>& settings);

  bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  ) override;

private:
  void doLayout(Renderer& renderer) override;

  float m_screenWidth = 0.0f;
  float m_screenHeight = 0.0f;
  float m_panelCenterY = 0.0f;
  std::unordered_map<std::string, WidgetSettingValue> m_settings;
  Box* m_panel = nullptr;
  Box* m_infoGhost = nullptr;
  Box* m_mediaGhost = nullptr;
  Box* m_weatherGhost = nullptr;
  Box* m_statusGhost = nullptr;
  Box* m_passwordGhost = nullptr;
  Box* m_loginButtonGhost = nullptr;
  Glyph* m_loginGlyph = nullptr;
  std::array<Box*, 5> m_sessionGhosts{};
};
