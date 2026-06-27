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
        if (desktop_widgets::outputKey(output) == outputName) {
          return desktop_widgets::outputLogicalWidth(output);
        }
      }
      return 1920.0f;
    }

  } // namespace

  bool isLoginBoxWidget(const DesktopWidgetState& state) { return state.type == kWidgetType; }

  bool isLoginBoxWidgetType(std::string_view type) { return type == kWidgetType; }

  bool isLoginBoxWidgetId(std::string_view id) { return id.starts_with(kWidgetIdPrefix); }

  std::string widgetIdForOutput(std::string_view outputKey) { return std::format("{}{}", kWidgetIdPrefix, outputKey); }

  float defaultPanelWidth(float screenWidth) {
    return std::min(screenWidth - Style::spaceLg * 2.0f, kDefaultPanelWidthCap);
  }

  float defaultPanelHeight() { return 70.0f; }

  float panelWidth(float screenWidth) { return defaultPanelWidth(screenWidth); }

  float panelHeight() { return defaultPanelHeight(); }

  float resolvePanelWidth(float screenWidth, float boxWidth) {
    if (boxWidth > 0.0f) {
      return std::clamp(boxWidth, kMinPanelWidth, screenWidth - Style::spaceLg * 2.0f);
    }
    return defaultPanelWidth(screenWidth);
  }

  float resolvePanelHeight(float boxHeight) {
    if (boxHeight > 0.0f) {
      return std::clamp(boxHeight, kMinPanelHeight, kMaxPanelHeight);
    }
    return defaultPanelHeight();
  }

  void defaultPanelSize(float screenWidth, float& boxWidth, float& boxHeight) {
    boxWidth = defaultPanelWidth(screenWidth);
    boxHeight = defaultPanelHeight();
  }

  void clampPanelSize(float screenWidth, float& boxWidth, float& boxHeight) {
    boxWidth = resolvePanelWidth(screenWidth, boxWidth);
    boxHeight = resolvePanelHeight(boxHeight);
  }

  PanelContentLayout panelContentLayout(float panelWidth, float panelHeight, bool showLoginButton) {
    PanelContentLayout layout;
    layout.contentLeft = Style::spaceLg;
    layout.controlHeight = std::min(Style::controlHeight, panelHeight - Style::spaceSm * 2.0f);
    layout.contentTop = std::max(Style::spaceSm, (panelHeight - layout.controlHeight) * 0.5f);
    const float rightInset = Style::spaceLg + Style::spaceSm;
    const float contentWidth = panelWidth - Style::spaceLg - rightInset;
    const float buttonWidth = layout.controlHeight;
    const float gap = Style::spaceSm;
    layout.inputWidth =
        showLoginButton ? std::max(120.0f, contentWidth - buttonWidth - gap) : std::max(120.0f, contentWidth);
    layout.buttonX = layout.contentLeft + layout.inputWidth + gap;
    return layout;
  }

  void defaultPanelCenter(float screenWidth, float screenHeight, float& cx, float& cy) {
    float width = defaultPanelWidth(screenWidth);
    float height = defaultPanelHeight();
    const float panelX = std::round((screenWidth - width) * 0.5f);
    const float panelY = std::max(Style::spaceLg, screenHeight - height - 84.0f);
    cx = panelX + width * 0.5f;
    cy = panelY + height * 0.5f;
  }

  void panelOriginFromCenter(
      float cx, float cy, float screenWidth, float boxWidth, float boxHeight, float& panelX, float& panelY,
      float& panelWidthOut, float& panelHeightOut
  ) {
    panelWidthOut = resolvePanelWidth(screenWidth, boxWidth);
    panelHeightOut = resolvePanelHeight(boxHeight);
    panelX = cx - panelWidthOut * 0.5f;
    panelY = cy - panelHeightOut * 0.5f;
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
    ColorSpec panelFill =
        colorSpecFromConfigString(readString(settings, "background_color", "surface_variant"), "background_color");
    panelFill.alpha *= std::clamp(readFloat(settings, "background_opacity", 0.88f), 0.0f, 1.0f);
    style.panelFill = panelFill;
    style.panelRadius = std::clamp(readFloat(settings, "background_radius", style.panelRadius), 0.0f, 32.0f);
    style.inputOpacity = std::clamp(readFloat(settings, kInputOpacityKey, style.inputOpacity), 0.0f, 1.0f);
    style.inputRadius = std::clamp(readFloat(settings, kInputRadiusKey, style.inputRadius), 0.0f, 32.0f);
    style.showLoginButton = readBool(settings, kShowLoginButtonKey, style.showLoginButton);
    return style;
  }

  void applyDefaultSettings(
      std::unordered_map<std::string, WidgetSettingValue>& settings, desktop_settings::DesktopWidgetSettingsScope scope
  ) {
    if (scope == desktop_settings::DesktopWidgetSettingsScope::Widget) {
      settings.insert_or_assign(std::string(kShowLoginButtonKey), true);
      settings.insert_or_assign(std::string(kInputOpacityKey), 1.0);
      settings.insert_or_assign(std::string(kInputRadiusKey), 6.0);
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
    if (!settings.contains(std::string(kShowLoginButtonKey))) {
      settings.insert_or_assign(std::string(kShowLoginButtonKey), true);
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
      widget.rotationRad = 0.0f;
      widget.enabled = true;
      widget.type = std::string(kWidgetType);
      normalizeSettings(widget.settings);
      const float screenWidth = screenWidthForOutput(wayland, widget.outputName);
      if (widget.boxWidth <= 0.0f || widget.boxHeight <= 0.0f) {
        defaultPanelSize(screenWidth, widget.boxWidth, widget.boxHeight);
      } else {
        clampPanelSize(screenWidth, widget.boxWidth, widget.boxHeight);
      }
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
      widget.rotationRad = 0.0f;
      widget.enabled = true;
      const float screenWidth = desktop_widgets::outputLogicalWidth(output);
      defaultPanelCenter(screenWidth, desktop_widgets::outputLogicalHeight(output), widget.cx, widget.cy);
      defaultPanelSize(screenWidth, widget.boxWidth, widget.boxHeight);
      applyDefaultSettings(widget.settings, desktop_settings::DesktopWidgetSettingsScope::Widget);
      applyDefaultSettings(widget.settings, desktop_settings::DesktopWidgetSettingsScope::Background);
      widgets.insert(widgets.begin(), std::move(widget));
      outputsWithLoginBox.insert(outputKey);
    }
  }

} // namespace lockscreen_login_box
