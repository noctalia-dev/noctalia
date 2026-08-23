#include "shell/lockscreen/lockscreen_login_box.h"

#include "shell/desktop/desktop_widget_layout.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

namespace lockscreen_login_box {

  namespace {

    [[nodiscard]] float readFloat(
        const std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key, float fallback
    ) {
      const auto it = settings.find(std::string(key));
      if (it == settings.end()) {
        return fallback;
      }
      if (const auto* value = std::get_if<double>(&it->second)) {
        return static_cast<float>(*value);
      }
      if (const auto* value = std::get_if<std::int64_t>(&it->second)) {
        return static_cast<float>(*value);
      }
      return fallback;
    }

    [[nodiscard]] bool
    readBool(const std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key, bool fallback) {
      const auto it = settings.find(std::string(key));
      if (it == settings.end()) {
        return fallback;
      }
      if (const auto* value = std::get_if<bool>(&it->second)) {
        return *value;
      }
      return fallback;
    }

    [[nodiscard]] std::string readString(
        const std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key,
        std::string_view fallback
    ) {
      const auto it = settings.find(std::string(key));
      if (it == settings.end()) {
        return std::string(fallback);
      }
      if (const auto* value = std::get_if<std::string>(&it->second)) {
        return *value;
      }
      return std::string(fallback);
    }

    void clampOpacitySetting(std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key) {
      const std::string keyStr(key);
      const auto it = settings.find(keyStr);
      if (it == settings.end()) {
        return;
      }
      if (const auto* doubleValue = std::get_if<double>(&it->second)) {
        settings.insert_or_assign(keyStr, std::clamp(*doubleValue, 0.0, 1.0));
        return;
      }
      if (const auto* intValue = std::get_if<std::int64_t>(&it->second)) {
        settings.insert_or_assign(keyStr, std::clamp(static_cast<double>(*intValue), 0.0, 1.0));
      }
    }

    void clampRadiusSetting(std::unordered_map<std::string, WidgetSettingValue>& settings, std::string_view key) {
      const std::string keyStr(key);
      const auto it = settings.find(keyStr);
      if (it == settings.end()) {
        return;
      }
      if (const auto* doubleValue = std::get_if<double>(&it->second)) {
        settings.insert_or_assign(keyStr, std::clamp(*doubleValue, 0.0, 32.0));
        return;
      }
      if (const auto* intValue = std::get_if<std::int64_t>(&it->second)) {
        settings.insert_or_assign(keyStr, std::clamp(static_cast<double>(*intValue), 0.0, 32.0));
      }
    }

    [[nodiscard]] float screenWidthForOutput(const WaylandConnection& wayland, std::string_view outputName) {
      for (const auto& output : wayland.outputs()) {
        if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
          continue;
        }
        if (desktop_widgets::outputKey(output) == outputName) {
          return desktop_widgets::outputLogicalWidth(output);
        }
      }
      return 1920.0F;
    }

  } // namespace

  bool isLoginBoxWidget(const DesktopWidgetState& state) { return state.type == kWidgetType; }

  bool isLoginBoxWidgetType(std::string_view type) { return type == kWidgetType; }

  bool isLoginBoxWidgetId(std::string_view id) { return id.starts_with(kWidgetIdPrefix); }

  std::string widgetIdForOutput(std::string_view outputKey) { return std::format("{}{}", kWidgetIdPrefix, outputKey); }

  LayoutMode resolveLayout(std::string_view layout) {
    if (layout == kLayoutCompact) {
      return LayoutMode::Compact;
    }
    return LayoutMode::Regular;
  }

  LayoutMode resolveLayout(const std::unordered_map<std::string, WidgetSettingValue>& settings) {
    return resolveLayout(readString(settings, kLayoutKey, kLayoutRegular));
  }

  InfoExtrasVisibility
  resolveInfoExtrasVisibility(bool regular, const LoginBoxStyle& style, bool mediaReady, bool weatherReady) {
    if (!regular) {
      return {};
    }
    const bool mediaConfigured = style.showMedia;
    const bool weatherConfigured = style.showWeather;
    // Hide an unavailable side when the other can fill the row. If both are configured
    // off, the info strip stays empty and is collapsed by the layout.
    const bool showWeather = weatherConfigured && (weatherReady || !mediaConfigured);
    const bool showMedia = mediaConfigured && (mediaReady || !showWeather);
    return InfoExtrasVisibility{.showMedia = showMedia, .showWeather = showWeather};
  }

  float infoExtraBudget(float contentWidth, bool showSelf, bool showOther) {
    if (!showSelf) {
      return 0.0F;
    }
    if (!showOther) {
      return contentWidth;
    }
    return std::max(0.0F, (contentWidth - Style::spaceMd) * 0.5F);
  }

  float minPanelWidth(LayoutMode layout) {
    return layout == LayoutMode::Regular ? kRegularMinPanelWidth : kCompactMinPanelWidth;
  }

  float regularInfoContentHeight() {
    // day + glyph + hi/lo — caption ink, not full controlHeightSm rows
    const float caption = Style::fontSizeCaption + Style::spaceXs;
    const float forecastColumn = caption + Style::spaceXs + kRegularForecastGlyphSize + Style::spaceXs + caption;
    return std::max(kRegularMediaArtSize, forecastColumn);
  }

  float regularStatusContentHeight() { return Style::spaceXs * 2.0F + Style::fontSizeCaption + Style::spaceXs; }

  float regularSessionContentHeight() { return Style::controlHeight; }

  RegularRowHeights regularRowHeights(float panelHeight, bool showSessionButtons, bool showInfoExtras) {
    const float infoFloor = showInfoExtras ? regularInfoContentHeight() : 0.0F;
    const float passwordFloor = Style::controlHeight;
    const float sessionFloor = showSessionButtons ? regularSessionContentHeight() : 0.0F;

    int rows = 1; // password
    if (showInfoExtras) {
      ++rows;
    }
    if (showSessionButtons) {
      ++rows;
    }
    const float pad = Style::spaceLg * 2.0F;
    const float gaps = Style::spaceSm * static_cast<float>(std::max(0, rows - 1));
    const float available = std::max(0.0F, panelHeight - pad - gaps);
    const float floors = infoFloor + passwordFloor + sessionFloor;
    const float scale = floors > 0.0F ? available / floors : 1.0F;

    return RegularRowHeights{
        .info = infoFloor * scale,
        .status = 0.0F,
        .password = passwordFloor * scale,
        .session = sessionFloor * scale,
        .scale = scale,
    };
  }

  bool styleShowsInfoExtras(const LoginBoxStyle& style) noexcept { return style.showMedia || style.showWeather; }

  float minPanelHeight(LayoutMode layout, bool showSessionButtons, bool showInfoExtras) {
    const float pad = Style::spaceLg * 2.0F;
    if (layout != LayoutMode::Regular) {
      return pad + Style::controlHeight;
    }

    int gapCount = 0;
    float height = pad + Style::controlHeight;
    if (showInfoExtras) {
      height += regularInfoContentHeight();
      ++gapCount;
    }
    if (showSessionButtons) {
      height += regularSessionContentHeight();
      ++gapCount;
    }
    height += Style::spaceSm * static_cast<float>(gapCount);
    return height;
  }

  float maxPanelHeight(LayoutMode layout) {
    return layout == LayoutMode::Regular ? kRegularMaxPanelHeight : kCompactMaxPanelHeight;
  }

  float defaultPanelWidth(float screenWidth, LayoutMode layout) {
    const float widthCap = layout == LayoutMode::Regular ? kRegularDefaultWidthCap : kCompactDefaultWidthCap;
    return std::min(screenWidth - Style::spaceLg * 2.0F, widthCap);
  }

  float defaultPanelHeight(LayoutMode layout, bool showSessionButtons, bool showInfoExtras) {
    if (layout != LayoutMode::Regular) {
      return minPanelHeight(layout, showSessionButtons, showInfoExtras);
    }
    return minPanelHeight(layout, showSessionButtons, showInfoExtras) + Style::spaceMd;
  }

  float resolvePanelWidth(float screenWidth, float boxWidth, LayoutMode layout) {
    const float minWidth = minPanelWidth(layout);
    if (boxWidth > 0.0F) {
      return std::clamp(boxWidth, minWidth, std::max(minWidth, screenWidth - Style::spaceLg * 2.0F));
    }
    return defaultPanelWidth(screenWidth, layout);
  }

  float resolvePanelHeight(float boxHeight, LayoutMode layout, bool showSessionButtons, bool showInfoExtras) {
    const float minHeight = minPanelHeight(layout, showSessionButtons, showInfoExtras);
    if (boxHeight > 0.0F) {
      return std::clamp(boxHeight, minHeight, maxPanelHeight(layout));
    }
    return defaultPanelHeight(layout, showSessionButtons, showInfoExtras);
  }

  void defaultPanelSize(
      float screenWidth, float& boxWidth, float& boxHeight, LayoutMode layout, bool showSessionButtons,
      bool showInfoExtras
  ) {
    boxWidth = defaultPanelWidth(screenWidth, layout);
    boxHeight = defaultPanelHeight(layout, showSessionButtons, showInfoExtras);
  }

  void clampPanelSize(
      float screenWidth, float& boxWidth, float& boxHeight, LayoutMode layout, bool showSessionButtons,
      bool showInfoExtras
  ) {
    boxWidth = resolvePanelWidth(screenWidth, boxWidth, layout);
    boxHeight = resolvePanelHeight(boxHeight, layout, showSessionButtons, showInfoExtras);
  }

  PanelContentLayout panelContentLayout(float panelWidth, float panelHeight, bool showLoginButton) {
    PanelContentLayout layout;
    layout.contentLeft = Style::spaceLg;
    // Center the input row vertically so the free space above and below the input is equal.
    layout.controlHeight = std::min(Style::controlHeight, panelHeight - Style::spaceLg * 2.0F);
    layout.contentTop = std::max(Style::spaceLg, (panelHeight - layout.controlHeight) * 0.5F);
    // Match the left inset so the padding left of the first control equals the padding right of the last.
    const float rightInset = Style::spaceLg;
    const float contentWidth = panelWidth - Style::spaceLg - rightInset;
    const float buttonWidth = layout.controlHeight;
    const float gap = Style::spaceSm;
    layout.inputWidth =
        showLoginButton ? std::max(120.0F, contentWidth - buttonWidth - gap) : std::max(120.0F, contentWidth);
    layout.buttonX = layout.contentLeft + layout.inputWidth + gap;
    return layout;
  }

  void defaultPanelCenter(
      float screenWidth, float screenHeight, float& cx, float& cy, LayoutMode layout, bool showSessionButtons,
      bool showInfoExtras
  ) {
    float width = defaultPanelWidth(screenWidth, layout);
    float height = defaultPanelHeight(layout, showSessionButtons, showInfoExtras);
    const float panelX = std::round((screenWidth - width) * 0.5F);
    const float panelY = std::max(Style::spaceLg, screenHeight - height - 84.0F);
    cx = panelX + width * 0.5F;
    cy = panelY + height * 0.5F;
  }

  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float boxWidth, float boxHeight, LayoutMode layout, float& panelX,
      float& panelY, float& panelWidthOut, float& panelHeightOut, bool showSessionButtons, bool showInfoExtras
  ) {
    panelWidthOut = resolvePanelWidth(screenWidth, boxWidth, layout);
    panelHeightOut = resolvePanelHeight(boxHeight, layout, showSessionButtons, showInfoExtras);
    panelX = cx - panelWidthOut * 0.5F;
    panelY = cy - panelHeightOut * 0.5F;
  }

  const DesktopWidgetState* findForOutput(const std::vector<DesktopWidgetState>& widgets, std::string_view outputKey) {
    for (const auto& widget : widgets) {
      if (!isLoginBoxWidget(widget)) {
        continue;
      }
      if (widget.outputName == outputKey) {
        return &widget;
      }
    }
    return nullptr;
  }

  LoginBoxStyle resolveStyle(const std::unordered_map<std::string, WidgetSettingValue>& settings) {
    LoginBoxStyle style;
    style.layout = resolveLayout(settings);
    style.panelOpacity = std::clamp(readFloat(settings, "background_opacity", style.panelOpacity), 0.0F, 1.0F);
    ColorSpec panelFill =
        colorSpecFromConfigString(readString(settings, "background_color", "surface_variant"), "background_color");
    panelFill.alpha *= style.panelOpacity;
    style.panelFill = panelFill;
    style.panelRadius = std::clamp(readFloat(settings, "background_radius", style.panelRadius), 0.0F, 32.0F);
    style.inputOpacity = std::clamp(readFloat(settings, kInputOpacityKey, style.inputOpacity), 0.0F, 1.0F);
    style.inputRadius = std::clamp(readFloat(settings, kInputRadiusKey, style.inputRadius), 0.0F, 32.0F);
    style.centerPasswordText = readBool(settings, kCenterPasswordTextKey, style.centerPasswordText);
    style.showLoginButton = readBool(settings, kShowLoginButtonKey, style.showLoginButton);
    style.showCapsLock = readBool(settings, kShowCapsLockKey, style.showCapsLock);
    style.showKeyboardLayout = readBool(settings, kShowKeyboardLayoutKey, style.showKeyboardLayout);
    style.showSessionButtons = readBool(settings, kShowSessionButtonsKey, style.showSessionButtons);
    style.showMedia = readBool(settings, kShowMediaKey, style.showMedia);
    style.showWeather = readBool(settings, kShowWeatherKey, style.showWeather);
    style.showUnlockHint = readBool(settings, kShowUnlockHintKey, style.showUnlockHint);
    return style;
  }

  void applyDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, desktop_settings::DesktopWidgetSettingsScope scope
  ) {
    if (scope == desktop_settings::DesktopWidgetSettingsScope::Widget) {
      settings.insert_or_assign(std::string(kLayoutKey), std::string(kLayoutRegular));
      settings.insert_or_assign(std::string(kShowSessionButtonsKey), true);
      settings.insert_or_assign(std::string(kShowMediaKey), true);
      settings.insert_or_assign(std::string(kShowWeatherKey), true);
      settings.insert_or_assign(std::string(kShowLoginButtonKey), true);
      settings.insert_or_assign(std::string(kShowCapsLockKey), true);
      settings.insert_or_assign(std::string(kShowKeyboardLayoutKey), true);
      settings.insert_or_assign(std::string(kShowUnlockHintKey), true);
      settings.insert_or_assign(std::string(kInputOpacityKey), 1.0);
      settings.insert_or_assign(std::string(kInputRadiusKey), 6.0);
      settings.insert_or_assign(std::string(kCenterPasswordTextKey), false);
    }
    if (scope == desktop_settings::DesktopWidgetSettingsScope::Background) {
      settings.insert_or_assign("background_color", std::string("surface_variant"));
      settings.insert_or_assign("background_opacity", 0.88);
      settings.insert_or_assign("background_radius", 12.0);
    }
  }

  void applyAllDefaultSettings(std::unordered_map<std::string, WidgetSettingValue>& settings) {
    applyDefaultSettings(settings, desktop_settings::DesktopWidgetSettingsScope::Widget);
    applyDefaultSettings(settings, desktop_settings::DesktopWidgetSettingsScope::Background);
  }

  void normalizeSettings(std::unordered_map<std::string, WidgetSettingValue>& settings) {
    if (!settings.contains(std::string(kLayoutKey))) {
      settings.insert_or_assign(std::string(kLayoutKey), std::string(kLayoutRegular));
    } else {
      const std::string layout = readString(settings, kLayoutKey, kLayoutRegular);
      if (layout != kLayoutCompact && layout != kLayoutRegular) {
        settings.insert_or_assign(std::string(kLayoutKey), std::string(kLayoutRegular));
      }
    }
    if (!settings.contains(std::string(kShowSessionButtonsKey))) {
      settings.insert_or_assign(std::string(kShowSessionButtonsKey), true);
    }
    if (!settings.contains(std::string(kShowMediaKey))) {
      settings.insert_or_assign(std::string(kShowMediaKey), true);
    }
    if (!settings.contains(std::string(kShowWeatherKey))) {
      settings.insert_or_assign(std::string(kShowWeatherKey), true);
    }
    if (!settings.contains(std::string(kShowLoginButtonKey))) {
      settings.insert_or_assign(std::string(kShowLoginButtonKey), true);
    }
    if (!settings.contains(std::string(kShowCapsLockKey))) {
      settings.insert_or_assign(std::string(kShowCapsLockKey), true);
    }
    if (!settings.contains(std::string(kShowKeyboardLayoutKey))) {
      settings.insert_or_assign(std::string(kShowKeyboardLayoutKey), true);
    }
    if (!settings.contains(std::string(kShowUnlockHintKey))) {
      settings.insert_or_assign(std::string(kShowUnlockHintKey), true);
    }
    if (!settings.contains(std::string(kCenterPasswordTextKey))) {
      settings.insert_or_assign(std::string(kCenterPasswordTextKey), false);
    }
    clampOpacitySetting(settings, "background_opacity");
    clampRadiusSetting(settings, "background_radius");
    clampOpacitySetting(settings, kInputOpacityKey);
    clampRadiusSetting(settings, kInputRadiusKey);
  }

  void ensureWidgets(std::vector<DesktopWidgetState>& widgets, const WaylandConnection& wayland) {
    std::unordered_set<std::string> outputsWithLoginBox;
    std::erase_if(widgets, [&](const DesktopWidgetState& widget) {
      if (!isLoginBoxWidget(widget)) {
        return false;
      }
      if (widget.outputName.empty() || outputsWithLoginBox.contains(widget.outputName)) {
        return true;
      }
      outputsWithLoginBox.insert(widget.outputName);
      return false;
    });

    for (auto& widget : widgets) {
      if (!isLoginBoxWidget(widget)) {
        continue;
      }
      widget.rotationRad = 0.0F;
      widget.type = std::string(kWidgetType);
      normalizeSettings(widget.settings);
      const LoginBoxStyle style = resolveStyle(widget.settings);
      const float screenWidth = screenWidthForOutput(wayland, widget.outputName);
      const bool showInfo = styleShowsInfoExtras(style);
      if (widget.boxWidth <= 0.0F || widget.boxHeight <= 0.0F) {
        defaultPanelSize(
            screenWidth, widget.boxWidth, widget.boxHeight, style.layout, style.showSessionButtons, showInfo
        );
      } else {
        // Keep width from the user; snap height to current chrome so stale tall boxes shrink.
        widget.boxHeight = defaultPanelHeight(style.layout, style.showSessionButtons, showInfo);
        clampPanelSize(
            screenWidth, widget.boxWidth, widget.boxHeight, style.layout, style.showSessionButtons, showInfo
        );
      }
      desktop_widgets::clampStateToOutput(wayland, widget, widget.boxWidth, widget.boxHeight);
    }

    for (const auto& output : wayland.outputs()) {
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      const std::string outputKey = desktop_widgets::outputKey(output);
      if (outputsWithLoginBox.contains(outputKey)) {
        continue;
      }

      DesktopWidgetState widget;
      widget.id = widgetIdForOutput(outputKey);
      widget.type = std::string(kWidgetType);
      widget.outputName = outputKey;
      widget.rotationRad = 0.0F;
      widget.enabled = true;
      const float screenWidth = desktop_widgets::outputLogicalWidth(output);
      applyDefaultSettings(widget.settings, desktop_settings::DesktopWidgetSettingsScope::Widget);
      applyDefaultSettings(widget.settings, desktop_settings::DesktopWidgetSettingsScope::Background);
      const LoginBoxStyle style = resolveStyle(widget.settings);
      const bool showInfo = styleShowsInfoExtras(style);
      defaultPanelCenter(
          screenWidth, desktop_widgets::outputLogicalHeight(output), widget.cx, widget.cy, style.layout,
          style.showSessionButtons, showInfo
      );
      defaultPanelSize(
          screenWidth, widget.boxWidth, widget.boxHeight, style.layout, style.showSessionButtons, showInfo
      );
      widgets.insert(widgets.begin(), std::move(widget));
      outputsWithLoginBox.insert(outputKey);
    }

    bool anyEnabled = false;
    for (const auto& widget : widgets) {
      if (isLoginBoxWidget(widget) && widget.enabled) {
        anyEnabled = true;
        break;
      }
    }
    if (!anyEnabled) {
      for (auto& widget : widgets) {
        if (isLoginBoxWidget(widget)) {
          widget.enabled = true;
          break;
        }
      }
    }
  }

} // namespace lockscreen_login_box
