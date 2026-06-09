#pragma once

#include "config/config_types.h"
#include "wayland/wayland_connection.h"

#include <string>
#include <string_view>
#include <vector>

namespace lockscreen_login_box {

  constexpr std::string_view kWidgetType = "login_box";
  constexpr std::string_view kWidgetIdPrefix = "lockscreen-login-box@";

  [[nodiscard]] bool isLoginBoxWidget(const DesktopWidgetState& state);
  [[nodiscard]] bool isLoginBoxWidgetType(std::string_view type);
  [[nodiscard]] bool isLoginBoxWidgetId(std::string_view id);
  [[nodiscard]] std::string widgetIdForOutput(std::string_view outputKey);

  [[nodiscard]] float panelWidth(float screenWidth, float loginScale = 1.0f);
  [[nodiscard]] float panelHeight(float loginScale = 1.0f);
  void defaultPanelCenter(float screenWidth, float screenHeight, float& cx, float& cy, float loginScale = 1.0f);
  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float loginScale, float& panelX, float& panelY, float& panelWidthOut
  );

  [[nodiscard]] const DesktopWidgetState*
  findForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey);

  void ensureWidgets(std::vector<DesktopWidgetState>& widgets, const WaylandConnection& wayland);

} // namespace lockscreen_login_box
