#include "shell/desktop/desktop_widget_factory.h"

#include "config/config_service.h"
#include "core/log.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_runtime_context.h"
#include "shell/desktop/widgets/desktop_audio_visualizer_widget.h"
#include "shell/desktop/widgets/desktop_button_widget.h"
#include "shell/desktop/widgets/desktop_clock_widget.h"
#include "shell/desktop/widgets/desktop_fancy_audio_visualizer_widget.h"
#include "shell/desktop/widgets/desktop_label_widget.h"
#include "shell/desktop/widgets/desktop_login_box_widget.h"
#include "shell/desktop/widgets/desktop_media_player_widget.h"
#include "shell/desktop/widgets/desktop_sticker_widget.h"
#include "shell/desktop/widgets/desktop_sysmon_widget.h"
#include "shell/desktop/widgets/desktop_volume_widget.h"
#include "shell/desktop/widgets/desktop_weather_widget.h"
#include "shell/desktop/widgets/plugin_desktop_widget.h"
#include "system/format_units.h"
#include "ui/controls/button.h"

#include <algorithm>
#include <optional>

namespace {

  constexpr Logger kLog("desktop");

  std::string getStringSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key,
      const std::string& fallback = {}
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return fallback;
    }
    if (const auto* value = std::get_if<std::string>(&it->second)) {
      return *value;
    }
    return fallback;
  }

  float getFloatSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key, float fallback
  ) {
    const auto it = settings.find(key);
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

  int getIntSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key, int fallback
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return fallback;
    }
    if (const auto* value = std::get_if<std::int64_t>(&it->second)) {
      return static_cast<int>(*value);
    }
    if (const auto* value = std::get_if<double>(&it->second)) {
      return static_cast<int>(*value);
    }
    return fallback;
  }

  bool getBoolSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key, bool fallback
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return fallback;
    }
    if (const auto* value = std::get_if<bool>(&it->second)) {
      return *value;
    }
    return fallback;
  }

  ColorSpec getColorSpecSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key,
      const ColorSpec& fallback
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return fallback;
    }
    if (const auto* value = std::get_if<std::string>(&it->second)) {
      return colorSpecFromConfigString(*value, key);
    }
    return fallback;
  }

  std::optional<ColorSpec> getOptionalColorSpecSetting(
      const std::unordered_map<std::string, WidgetSettingValue>& settings, const std::string& key
  ) {
    const auto it = settings.find(key);
    if (it == settings.end()) {
      return std::nullopt;
    }
    const auto* value = std::get_if<std::string>(&it->second);
    if (value == nullptr || value->empty()) {
      return std::nullopt;
    }
    return colorSpecFromConfigString(*value, key);
  }

  constexpr float kDefaultBgRadius = 12.0f;
  constexpr float kDefaultBgPadding = 10.0f;

  std::optional<FancyAudioVisualizerMode>
  getFancyAudioVisualizerMode(const std::unordered_map<std::string, WidgetSettingValue>& settings) {
    const auto it = settings.find("visualization_mode");
    if (it == settings.end()) {
      return FancyAudioVisualizerMode::BarsRings;
    }
    const auto* value = std::get_if<std::string>(&it->second);
    if (value == nullptr) {
      kLog.warn("desktop widget factory: fancy_audio_visualizer visualization_mode must be a string");
      return std::nullopt;
    }
    if (*value == "bars")
      return FancyAudioVisualizerMode::Bars;
    if (*value == "wave")
      return FancyAudioVisualizerMode::Wave;
    if (*value == "rings")
      return FancyAudioVisualizerMode::Rings;
    if (*value == "bars_rings")
      return FancyAudioVisualizerMode::BarsRings;
    if (*value == "wave_rings")
      return FancyAudioVisualizerMode::WaveRings;
    if (*value == "all")
      return FancyAudioVisualizerMode::All;
    kLog.warn("desktop widget factory: invalid fancy_audio_visualizer visualization_mode '{}'", *value);
    return std::nullopt;
  }

  ButtonVariant getButtonVariant(const std::unordered_map<std::string, WidgetSettingValue>& settings) {
    const std::string value = getStringSetting(settings, "variant", "default");
    if (value == "primary") {
      return ButtonVariant::Primary;
    }
    if (value == "secondary") {
      return ButtonVariant::Secondary;
    }
    if (value == "destructive") {
      return ButtonVariant::Destructive;
    }
    if (value == "outline") {
      return ButtonVariant::Outline;
    }
    if (value == "ghost") {
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

  void applyCommonSettings(
      DesktopWidget& widget, const std::unordered_map<std::string, WidgetSettingValue>& settings,
      bool defaultBackground = true
  ) {
    if (getBoolSetting(settings, "background", defaultBackground)) {
      ColorSpec bgColor = getColorSpecSetting(settings, "background_color", colorSpecFromRole(ColorRole::Surface));
      bgColor.alpha *= std::clamp(getFloatSetting(settings, "background_opacity", 0.8f), 0.0f, 1.0f);
      const float radius = getFloatSetting(settings, "background_radius", kDefaultBgRadius);
      const float padding = getFloatSetting(settings, "background_padding", kDefaultBgPadding);
      widget.setBackgroundStyle(bgColor, radius, padding);
    }
    // Stored on the widget and pushed onto its text nodes during layout(), so the chosen font
    // survives widget rebuilds (constructor settings bake in; this does not).
    widget.setFontFamily(getStringSetting(settings, "font_family", ""));
  }

} // namespace

DesktopWidgetFactory::DesktopWidgetFactory(DesktopWidgetRuntimeServices services)
    : m_pipewire(services.pipewire), m_pipewireSpectrum(services.pipewireSpectrum), m_weather(services.weather),
      m_mpris(services.mpris), m_httpClient(services.httpClient), m_sysmon(services.sysmon),
      m_scriptDeps(services.scriptDeps) {}

std::unique_ptr<DesktopWidget> DesktopWidgetFactory::create(
    const std::string& type, const std::unordered_map<std::string, WidgetSettingValue>& settings, float contentScale
) const {
  if (type == "clock") {
    const std::string styleSetting = getStringSetting(settings, "clock_style", "digital");
    const DesktopClockWidget::Style style =
        styleSetting == "analog" ? DesktopClockWidget::Style::Analog : DesktopClockWidget::Style::Digital;
    auto widget = std::make_unique<DesktopClockWidget>(DesktopClockWidget::Options{
        .style = style,
        .format = getStringSetting(settings, "format", "{:%H:%M}"),
        .color = getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
        .shadow = getBoolSetting(settings, "shadow", true),
        .showCircle = getBoolSetting(settings, "circle", true),
        .centerText = getBoolSetting(settings, "center_text", false),
        .timezone = getStringSetting(settings, "timezone", ""),
    });
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "audio_visualizer") {
    if (m_pipewireSpectrum == nullptr) {
      kLog.warn("desktop widget factory: audio_visualizer requires PipeWireSpectrum");
      return nullptr;
    }
    auto widget = std::make_unique<DesktopAudioVisualizerWidget>(
        m_pipewireSpectrum,
        DesktopAudioVisualizerWidget::Options{
            .bands = getIntSetting(settings, "bands", 32),
            .mirrored = getBoolSetting(settings, "mirrored", true),
            .centered = getBoolSetting(settings, "centered", true),
            .showWhenIdle = getBoolSetting(settings, "show_when_idle", true),
            .color1 = getColorSpecSetting(settings, "color_1", colorSpecFromRole(ColorRole::Primary)),
            .color2 = getColorSpecSetting(settings, "color_2", colorSpecFromRole(ColorRole::Primary)),
        }
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "fancy_audio_visualizer") {
    if (m_pipewireSpectrum == nullptr) {
      kLog.warn("desktop widget factory: fancy_audio_visualizer requires PipeWireSpectrum");
      return nullptr;
    }
    const auto mode = getFancyAudioVisualizerMode(settings);
    if (!mode.has_value()) {
      return nullptr;
    }
    auto widget = std::make_unique<DesktopFancyAudioVisualizerWidget>(
        m_pipewireSpectrum,
        DesktopFancyAudioVisualizerWidget::Options{
            .mode = *mode,
            .sensitivity = getFloatSetting(settings, "sensitivity", 1.5f),
            .rotationSpeed = getFloatSetting(settings, "rotation_speed", 0.5f),
            .barWidth = getFloatSetting(settings, "bar_width", 0.6f),
            .ringOpacity = getFloatSetting(settings, "ring_opacity", 0.8f),
            .bloomIntensity = getFloatSetting(settings, "bloom_intensity", 0.5f),
            .waveThickness = getFloatSetting(settings, "wave_thickness", 1.0f),
            .innerDiameter = getFloatSetting(settings, "inner_diameter", 0.7f),
            .fadeWhenIdle = getBoolSetting(settings, "fade_when_idle", true),
            .primaryColor = getColorSpecSetting(settings, "primary_color", colorSpecFromRole(ColorRole::Primary)),
            .secondaryColor = getColorSpecSetting(settings, "secondary_color", colorSpecFromRole(ColorRole::Secondary)),
        }
    );
    applyCommonSettings(*widget, settings, false);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "sticker") {
    auto widget = std::make_unique<DesktopStickerWidget>(
        getStringSetting(settings, "image_path"), std::clamp(getFloatSetting(settings, "opacity", 1.0f), 0.0f, 1.0f)
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "weather") {
    if (m_weather == nullptr) {
      kLog.warn("desktop widget factory: weather requires WeatherService");
      return nullptr;
    }
    auto widget = std::make_unique<DesktopWeatherWidget>(
        m_weather,
        DesktopWeatherWidget::Options{
            .color = getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
            .shadow = getBoolSetting(settings, "shadow", true),
            .showForecast = getBoolSetting(settings, "show_forecast", false),
            .forecastDays = getIntSetting(settings, "forecast_days", 3),
        }
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "media_player") {
    if (m_mpris == nullptr) {
      kLog.warn("desktop widget factory: media_player requires MprisService");
      return nullptr;
    }
    auto widget = std::make_unique<DesktopMediaPlayerWidget>(
        m_mpris, m_httpClient,
        DesktopMediaPlayerWidget::Options{
            .vertical = getStringSetting(settings, "layout", "horizontal") == "vertical",
            .color = getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
            .shadow = getBoolSetting(settings, "shadow", true),
            .hideWhenNoMedia = getBoolSetting(settings, "hide_when_no_media", false),
        }
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "label") {
    auto widget = std::make_unique<DesktopLabelWidget>(DesktopLabelWidget::Options{
        .title = getStringSetting(settings, "title", "Title"),
        .description = getStringSetting(settings, "description"),
        .color = getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
        .opacity = std::clamp(getFloatSetting(settings, "opacity", 1.0f), 0.0f, 1.0f),
        .shadow = getBoolSetting(settings, "shadow", true),
    });
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "button") {
    auto widget = std::make_unique<DesktopButtonWidget>(DesktopButtonWidget::Options{
        .glyph = getStringSetting(settings, "glyph"),
        .label = getStringSetting(settings, "label"),
        .command = getStringSetting(settings, "command"),
        .variant = getButtonVariant(settings),
        .showBackground = getBoolSetting(settings, "background", true),
        .labelColor = getOptionalColorSpecSetting(settings, "color"),
        .hoverBackground = getColorSpecSetting(settings, "hover_background", colorSpecFromRole(ColorRole::Hover)),
    });
    widget->setFontFamily(getStringSetting(settings, "font_family", ""));
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "sysmon") {
    if (m_sysmon == nullptr) {
      kLog.warn("desktop widget factory: sysmon requires SystemMonitorService");
      return nullptr;
    }
    auto parseStat = [](const std::string& s) -> DesktopSysmonStat {
      if (s == "cpu_temp")
        return DesktopSysmonStat::CpuTemp;
      if (s == "gpu_temp")
        return DesktopSysmonStat::GpuTemp;
      if (s == "gpu_usage")
        return DesktopSysmonStat::GpuUsage;
      if (s == "gpu_vram")
        return DesktopSysmonStat::GpuVram;
      if (s == "ram_pct")
        return DesktopSysmonStat::RamPct;
      if (s == "swap_pct")
        return DesktopSysmonStat::SwapPct;
      if (s == "net_rx")
        return DesktopSysmonStat::NetRx;
      if (s == "net_tx")
        return DesktopSysmonStat::NetTx;
      return DesktopSysmonStat::CpuUsage;
    };
    const DesktopSysmonStat stat = parseStat(getStringSetting(settings, "stat", "cpu_usage"));
    const std::string displayStr = getStringSetting(settings, "display", "graph");
    const DesktopSysmonDisplayMode displayMode =
        displayStr == "gauge" ? DesktopSysmonDisplayMode::Gauge : DesktopSysmonDisplayMode::Graph;
    const std::string gaugeLayoutStr = getStringSetting(settings, "gauge_layout", "horizontal");
    const DesktopSysmonGaugeLayout gaugeLayout =
        gaugeLayoutStr == "vertical" ? DesktopSysmonGaugeLayout::Vertical : DesktopSysmonGaugeLayout::Horizontal;
    std::optional<DesktopSysmonStat> stat2;
    if (displayMode == DesktopSysmonDisplayMode::Graph) {
      const std::string stat2Str = getStringSetting(settings, "stat2");
      if (!stat2Str.empty()) {
        stat2 = parseStat(stat2Str);
      }
    }
    auto widget = std::make_unique<DesktopSysmonWidget>(
        m_sysmon,
        DesktopSysmonWidget::Options{
            .stat = stat,
            .stat2 = stat2,
            .displayMode = displayMode,
            .gaugeLayout = gaugeLayout,
            .lineColor = getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::Primary)),
            .lineColor2 = getColorSpecSetting(settings, "color2", colorSpecFromRole(ColorRole::Secondary)),
            .highlightColor = getColorSpecSetting(settings, "highlight_color", colorSpecFromRole(ColorRole::Error)),
            .networkInterface = getStringSetting(settings, "interface"),
            .networkSpeedUnit =
                FormatUnits::decimalByteRateUnitFromString(getStringSetting(settings, "network_speed_unit", "auto")),
            .networkSpeedLabelStyle = getBoolSetting(settings, "network_speed_compact", false)
                ? FormatUnits::ByteRateLabelStyle::Compact
                : FormatUnits::ByteRateLabelStyle::Full,
            .showLabel = getBoolSetting(settings, "show_label", true),
            .labelMinWidth = getFloatSetting(settings, "label_min_width", 0.0f),
            .shadow = getBoolSetting(settings, "shadow", true),
            .config = m_scriptDeps.configService,
        }
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "volume") {
    if (m_pipewire == nullptr) {
      kLog.warn("desktop widget factory: volume requires PipeWireService");
      return nullptr;
    }
    auto widget = std::make_unique<DesktopVolumeWidget>(
        m_pipewire,
        DesktopVolumeWidget::Options{
            .glyph = getStringSetting(settings, "glyph"),
            .fillColor = getColorSpecSetting(settings, "fill_color", colorSpecFromRole(ColorRole::Primary)),
            .trackColor = getColorSpecSetting(settings, "track_color", colorSpecFromRole(ColorRole::OnSurfaceVariant)),
            .input = getStringSetting(settings, "device", "output") == "input",
            .showDevice = getBoolSetting(settings, "show_device", true),
            .shadow = getBoolSetting(settings, "shadow", true),
            .scrollStepPercent = getIntSetting(settings, "scroll_step", 5),
            .config = m_scriptDeps.configService,
        }
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (type == "login_box") {
    auto widget = std::make_unique<DesktopLoginBoxWidget>();
    widget->setSettings(settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  if (auto pluginEntry = scripting::PluginRegistry::instance().resolve(type);
      pluginEntry.has_value() && pluginEntry->entry->kind == scripting::PluginEntryKind::DesktopWidget) {
    if (m_scriptDeps.scriptApi == nullptr) {
      kLog.warn("desktop widget factory: plugin widget \"{}\" requires script support in this host", type);
      return nullptr;
    }
    auto seeded = scripting::seedEntrySettings(*pluginEntry->entry, settings);
    static const std::unordered_map<std::string, WidgetSettingValue> kNoPluginOverrides;
    const auto* pluginOverrides = &kNoPluginOverrides;
    if (m_scriptDeps.configService != nullptr) {
      const auto& pluginSettings = m_scriptDeps.configService->config().plugins.pluginSettings;
      if (const auto psIt = pluginSettings.find(pluginEntry->manifest->id); psIt != pluginSettings.end()) {
        pluginOverrides = &psIt->second;
      }
    }
    scripting::mergePluginSettings(*pluginEntry->manifest, *pluginOverrides, seeded);
    auto widget = std::make_unique<PluginDesktopWidget>(
        scripting::PluginRuntimeContext{
            .entryId = pluginEntry->fullId(),
            .sourcePath = pluginEntry->sourcePath,
            .settings = std::move(seeded),
            .scriptApi = *m_scriptDeps.scriptApi,
            .fileWatcher = m_scriptDeps.fileWatcher,
            .httpClient = m_httpClient,
            .clipboard = m_scriptDeps.clipboard,
        },
        std::string{}
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  kLog.warn("desktop widget factory: unknown widget type \"{}\"", type);
  return nullptr;
}
