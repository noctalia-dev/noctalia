#include "shell/desktop/desktop_widget_factory.h"

#include "core/log.h"
#include "pipewire/pipewire_spectrum.h"
#include "shell/desktop/widgets/desktop_audio_visualizer_widget.h"
#include "shell/desktop/widgets/desktop_clock_widget.h"
#include "shell/desktop/widgets/desktop_media_player_widget.h"
#include "shell/desktop/widgets/desktop_sticker_widget.h"
#include "shell/desktop/widgets/desktop_sysmon_widget.h"
#include "shell/desktop/widgets/desktop_weather_widget.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("desktop");
  constexpr float kDefaultDesktopAudioVisualizerAspectRatio = 240.0f / 96.0f;

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

  constexpr float kDefaultBgRadius = 12.0f;
  constexpr float kDefaultBgPadding = 10.0f;

  void applyCommonSettings(DesktopWidget& widget, const std::unordered_map<std::string, WidgetSettingValue>& settings) {
    if (getBoolSetting(settings, "background", true)) {
      ColorSpec bgColor = getColorSpecSetting(settings, "background_color", colorSpecFromRole(ColorRole::Surface));
      bgColor.alpha *= std::clamp(getFloatSetting(settings, "background_opacity", 0.8f), 0.0f, 1.0f);
      const float radius = getFloatSetting(settings, "background_radius", kDefaultBgRadius);
      const float padding = getFloatSetting(settings, "background_padding", kDefaultBgPadding);
      widget.setBackgroundStyle(bgColor, radius, padding);
    }
  }

} // namespace

DesktopWidgetFactory::DesktopWidgetFactory(
    PipeWireSpectrum* pipewireSpectrum, const WeatherService* weather, MprisService* mpris, HttpClient* httpClient,
    SystemMonitorService* sysmon
)
    : m_pipewireSpectrum(pipewireSpectrum), m_weather(weather), m_mpris(mpris), m_httpClient(httpClient),
      m_sysmon(sysmon) {}

std::unique_ptr<DesktopWidget> DesktopWidgetFactory::create(
    const std::string& type, const std::unordered_map<std::string, WidgetSettingValue>& settings, float contentScale
) const {
  if (type == "clock") {
    auto widget = std::make_unique<DesktopClockWidget>(
        getStringSetting(settings, "format", "{:%H:%M}"),
        getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
        getBoolSetting(settings, "shadow", true)
    );
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
        m_pipewireSpectrum, getFloatSetting(settings, "aspect_ratio", kDefaultDesktopAudioVisualizerAspectRatio),
        getIntSetting(settings, "bands", 32), getBoolSetting(settings, "mirrored", true),
        getColorSpecSetting(settings, "low_color", colorSpecFromRole(ColorRole::Primary)),
        getColorSpecSetting(settings, "high_color", colorSpecFromRole(ColorRole::Primary)),
        getBoolSetting(settings, "centered", true), getBoolSetting(settings, "show_when_idle", true)
    );
    applyCommonSettings(*widget, settings);
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
        m_weather, getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
        getBoolSetting(settings, "shadow", true)
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
    const bool vertical = getStringSetting(settings, "layout", "horizontal") == "vertical";
    auto widget = std::make_unique<DesktopMediaPlayerWidget>(
        m_mpris, m_httpClient, vertical,
        getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::OnSurface)),
        getBoolSetting(settings, "shadow", true)
    );
    applyCommonSettings(*widget, settings);
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
    const std::string stat2Str = getStringSetting(settings, "stat2");
    std::optional<DesktopSysmonStat> stat2;
    if (!stat2Str.empty()) {
      stat2 = parseStat(stat2Str);
    }
    auto widget = std::make_unique<DesktopSysmonWidget>(
        m_sysmon, stat, stat2, getColorSpecSetting(settings, "color", colorSpecFromRole(ColorRole::Primary)),
        getColorSpecSetting(settings, "color2", colorSpecFromRole(ColorRole::Secondary)),
        getBoolSetting(settings, "show_label", true), getBoolSetting(settings, "shadow", true)
    );
    applyCommonSettings(*widget, settings);
    widget->setContentScale(contentScale);
    return widget;
  }

  kLog.warn("desktop widget factory: unknown widget type \"{}\"", type);
  return nullptr;
}
