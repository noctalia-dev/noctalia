#pragma once

#include "config/config_types.h"
#include "ui/palette.h"

#include <cstdint>
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

  constexpr std::string_view kLayoutKey = "layout";
  constexpr std::string_view kLayoutCompact = "compact";
  constexpr std::string_view kLayoutRegular = "regular";
  constexpr std::string_view kShowSessionButtonsKey = "show_session_buttons";
  constexpr std::string_view kShowMediaKey = "show_media";
  constexpr std::string_view kShowWeatherKey = "show_weather";
  constexpr std::string_view kInputOpacityKey = "input_opacity";
  constexpr std::string_view kInputRadiusKey = "input_radius";
  constexpr std::string_view kCenterPasswordTextKey = "center_password_text";
  constexpr std::string_view kShowLoginButtonKey = "show_login_button";
  constexpr std::string_view kShowCapsLockKey = "show_caps_lock";
  constexpr std::string_view kShowKeyboardLayoutKey = "show_keyboard_layout";
  constexpr std::string_view kShowUnlockHintKey = "show_unlock_hint";

  enum class LayoutMode : std::uint8_t {
    Compact,
    Regular,
  };

  struct LoginBoxStyle {
    LayoutMode layout = LayoutMode::Regular;
    ColorSpec panelFill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.88f);
    float panelOpacity = 0.88f;
    float panelRadius = 12.0f;
    float inputOpacity = 1.0f;
    float inputRadius = 6.0f;
    bool centerPasswordText = false;
    bool showLoginButton = true;
    bool showCapsLock = true;
    bool showKeyboardLayout = true;
    bool showSessionButtons = true;
    bool showMedia = true;
    bool showWeather = true;
    bool showUnlockHint = true;
  };

  // Regular info row media/weather visibility (both may be off).
  struct InfoExtrasVisibility {
    bool showMedia = false;
    bool showWeather = false;
  };

  [[nodiscard]] InfoExtrasVisibility
  resolveInfoExtrasVisibility(bool regular, const LoginBoxStyle& style, bool mediaReady, bool weatherReady);
  [[nodiscard]] float infoExtraBudget(float contentWidth, bool showSelf, bool showOther);

  [[nodiscard]] bool isLoginBoxWidget(const DesktopWidgetState& state);
  [[nodiscard]] bool isLoginBoxWidgetType(std::string_view type);
  [[nodiscard]] bool isLoginBoxWidgetId(std::string_view id);
  [[nodiscard]] std::string widgetIdForOutput(std::string_view outputKey);

  [[nodiscard]] LayoutMode resolveLayout(const std::unordered_map<std::string, WidgetSettingValue>& settings);
  [[nodiscard]] LayoutMode resolveLayout(std::string_view layout);

  constexpr float kCompactDefaultWidthCap = 400.0f;
  constexpr float kRegularDefaultWidthCap = 810.0f;
  constexpr float kCompactMinPanelWidth = 240.0f;
  // Min width for media + weather; forecast needs more.
  constexpr float kRegularMinPanelWidth = 720.0f;
  constexpr float kCompactMaxPanelHeight = 140.0f;
  constexpr float kRegularMaxPanelHeight = 320.0f;

  // Matches lock-surface media art / forecast glyph sizes used in Regular layout.
  constexpr float kRegularMediaArtSize = 40.0f;
  constexpr float kRegularForecastGlyphSize = 18.0f;

  // Content floors shared by min-size clamping and the editor ghost.
  [[nodiscard]] float regularInfoContentHeight();
  [[nodiscard]] float regularStatusContentHeight();
  [[nodiscard]] float regularSessionContentHeight();

  // Proportional row heights for Regular: each floor scales by the same factor.
  struct RegularRowHeights {
    float info = 0.0f;
    float status = 0.0f;
    float password = 0.0f;
    float session = 0.0f;
    float scale = 1.0f;
  };

  [[nodiscard]] RegularRowHeights
  regularRowHeights(float panelHeight, bool showSessionButtons, bool showInfoExtras = true);

  struct PanelContentLayout {
    float contentLeft = 0.0f;
    float contentTop = 0.0f;
    float inputWidth = 0.0f;
    float buttonX = 0.0f;
    float controlHeight = 0.0f;
  };

  [[nodiscard]] float defaultPanelWidth(float screenWidth, LayoutMode layout);
  [[nodiscard]] float defaultPanelHeight(LayoutMode layout, bool showSessionButtons = true, bool showInfoExtras = true);
  [[nodiscard]] float minPanelWidth(LayoutMode layout);
  [[nodiscard]] float minPanelHeight(LayoutMode layout, bool showSessionButtons = true, bool showInfoExtras = true);
  [[nodiscard]] float maxPanelHeight(LayoutMode layout);
  [[nodiscard]] float resolvePanelWidth(float screenWidth, float boxWidth, LayoutMode layout);
  [[nodiscard]] float
  resolvePanelHeight(float boxHeight, LayoutMode layout, bool showSessionButtons = true, bool showInfoExtras = true);
  void defaultPanelSize(
      float screenWidth, float& boxWidth, float& boxHeight, LayoutMode layout, bool showSessionButtons = true,
      bool showInfoExtras = true
  );
  void clampPanelSize(
      float screenWidth, float& boxWidth, float& boxHeight, LayoutMode layout, bool showSessionButtons = true,
      bool showInfoExtras = true
  );
  [[nodiscard]] PanelContentLayout panelContentLayout(float panelWidth, float panelHeight, bool showLoginButton);
  void defaultPanelCenter(
      float screenWidth, float screenHeight, float& cx, float& cy, LayoutMode layout, bool showSessionButtons = true,
      bool showInfoExtras = true
  );
  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float boxWidth, float boxHeight, LayoutMode layout, float& panelX,
      float& panelY, float& panelWidthOut, float& panelHeightOut, bool showSessionButtons = true,
      bool showInfoExtras = true
  );

  // Height flags from settings.
  [[nodiscard]] bool styleShowsInfoExtras(const LoginBoxStyle& style) noexcept;

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
