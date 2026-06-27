#pragma once

#include "config/config_types.h"
#include "ui/palette.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class WaylandConnection;

namespace desktop_settings {
  enum class DesktopWidgetSettingsScope;
}

namespace lockscreen_login_box {

  constexpr std::string_view kWidgetType = "login_box";
  constexpr std::string_view kWidgetIdPrefix = "lockscreen-login-box@";

  constexpr std::string_view kInputOpacityKey = "input_opacity";
  constexpr std::string_view kInputRadiusKey = "input_radius";
  constexpr std::string_view kShowLoginButtonKey = "show_login_button";

  struct LoginBoxStyle {
    ColorSpec panelFill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.88f);
    float panelRadius = 12.0f;
    float inputOpacity = 1.0f;
    float inputRadius = 6.0f;
    bool showLoginButton = true;
  };

  [[nodiscard]] bool isLoginBoxWidget(const DesktopWidgetState& state);
  [[nodiscard]] bool isLoginBoxWidgetType(std::string_view type);
  [[nodiscard]] bool isLoginBoxWidgetId(std::string_view id);
  [[nodiscard]] std::string widgetIdForOutput(std::string_view outputKey);

  constexpr float kDefaultPanelWidthCap = 400.0f;
  constexpr float kMinPanelWidth = 240.0f;
  constexpr float kMinPanelHeight = 64.0f;
  constexpr float kMaxPanelHeight = 120.0f;

  struct PanelContentLayout {
    float contentLeft = 0.0f;
    float contentTop = 0.0f;
    float inputWidth = 0.0f;
    float buttonX = 0.0f;
    float controlHeight = 0.0f;
  };

  [[nodiscard]] float defaultPanelWidth(float screenWidth);
  [[nodiscard]] float defaultPanelHeight();
  [[nodiscard]] float panelWidth(float screenWidth);
  [[nodiscard]] float panelHeight();
  [[nodiscard]] float resolvePanelWidth(float screenWidth, float boxWidth);
  [[nodiscard]] float resolvePanelHeight(float boxHeight);
  void defaultPanelSize(float screenWidth, float& boxWidth, float& boxHeight);
  void clampPanelSize(float screenWidth, float& boxWidth, float& boxHeight);
  [[nodiscard]] PanelContentLayout panelContentLayout(float panelWidth, float panelHeight, bool showLoginButton);
  void defaultPanelCenter(float screenWidth, float screenHeight, float& cx, float& cy);
  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float boxWidth, float boxHeight, float& panelX, float& panelY,
      float& panelWidthOut, float& panelHeightOut
  );

  [[nodiscard]] const DesktopWidgetState*
  findForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey);

  [[nodiscard]] LoginBoxStyle resolveStyle(const std::unordered_map<std::string, WidgetSettingValue>& settings);
  void applyDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, desktop_settings::DesktopWidgetSettingsScope scope
  );
  void applyAllDefaultSettings(std::unordered_map<std::string, WidgetSettingValue>& settings);
  void normalizeSettings(std::unordered_map<std::string, WidgetSettingValue>& settings);

  void ensureWidgets(std::vector<DesktopWidgetState>& widgets, const WaylandConnection& wayland);

} // namespace lockscreen_login_box
