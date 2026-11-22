#include "shell/desktop/widgets/desktop_weather_widget.h"

#include "i18n/i18n.h"
#include "system/weather_service.h"
#include "time/time_format.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <memory>
#include <utility>
namespace {
  std::string todayIso(std::int32_t utcOffsetSeconds) {
    const auto now = std::chrono::system_clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    return formatStrftime("%Y-%m-%d", tm);
  }

  std::string weekdayAbbrev(const std::string& isoDate) {
    if (isoDate.size() != 10) {
      return isoDate;
    }

    std::tm tm{};
    tm.tm_year = std::stoi(isoDate.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(isoDate.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(isoDate.substr(8, 2));
    if (std::mktime(&tm) == -1) {
      return isoDate;
    }

    const std::string weekday = formatStrftime("%a", tm);
    return weekday.empty() ? isoDate : weekday;
  }

} // namespace
DesktopWeatherWidget::DesktopWeatherWidget(const WeatherService* weather, Options options)
    : m_weather(weather), m_color(options.color), m_shadow(options.shadow), m_showForecast(options.showForecast),
      m_forecastDays(std::clamp(options.forecastDays, 1, static_cast<int>(DesktopWeather::kMaxForecastRows))) {}

void DesktopWeatherWidget::create() {
  auto widget = std::make_unique<DesktopWeather>();
  m_widget = widget.get();
  widget->setColor(m_color);
  widget->setShadow(m_shadow);
  widget->setForecast(m_showForecast, m_forecastDays);
  setRoot(std::move(widget));
}
bool DesktopWeatherWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      m_widget->setColor(m_color);
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      m_widget->setShadow(m_shadow);
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "show_forecast") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showForecast = *v;
      m_widget->setForecast(m_showForecast, m_forecastDays);
      sync();
      syncForecast();
      m_widget->setData(m_data);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  if (key == "forecast_days") {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      m_forecastDays = std::clamp(static_cast<int>(*v), 1, static_cast<int>(DesktopWeather::kMaxForecastRows));
      m_widget->setForecast(m_showForecast, m_forecastDays);
      syncForecast();
      m_widget->setData(m_data);
      layout(renderer);
      (void)allSettings;
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}
void DesktopWeatherWidget::onFontFamilyChanged(const std::string& family, Renderer&) {
  if (m_widget != nullptr) {
    m_widget->setFontFamily(family);
  }
}
void DesktopWeatherWidget::doLayout(Renderer& renderer) {
  if (m_widget == nullptr) {
    return;
  }
  sync();
  syncForecast();
  m_widget->setData(m_data);
  m_widget->setContentScale(contentScale());
  m_widget->layout(renderer);
}
void DesktopWeatherWidget::doUpdate(Renderer&) {
  bool changed = sync();
  changed = syncForecast() || changed;
  if (m_widget != nullptr) {
    m_widget->setData(m_data);
  }
  if (changed) {
    requestLayout();
  }
}
bool DesktopWeatherWidget::sync() {
  std::string glyphName = "weather-cloud";
  std::string temperatureText = "--";
  std::string conditionText;

  if (m_weather == nullptr || !m_weather->enabled()) {
    temperatureText = i18n::tr("desktop-widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    temperatureText = i18n::tr("desktop-widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    glyphName = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    temperatureText = std::format("{}{}", temp, m_weather->displayTemperatureUnit());
    conditionText = WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
  } else if (m_weather->loading()) {
    temperatureText = i18n::tr("desktop-widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    temperatureText = i18n::tr("desktop-widgets.weather.error");
  }

  DesktopWeather::Data data;
  data.glyph = std::move(glyphName);
  data.temperature = std::move(temperatureText);
  data.condition = std::move(conditionText);
  const bool currentChanged =
      data.glyph != m_data.glyph || data.temperature != m_data.temperature || data.condition != m_data.condition;
  data.forecast = m_data.forecast;
  m_data = std::move(data);
  return currentChanged;
}
bool DesktopWeatherWidget::syncForecast() {
  const int rowCount =
      m_showForecast ? std::clamp(m_forecastDays, 1, static_cast<int>(DesktopWeather::kMaxForecastRows)) : 0;
  const bool hasWeatherData = m_weather != nullptr && m_weather->hasData();
  std::vector<DesktopWeather::Forecast> data;

  for (std::size_t i = 0; i < DesktopWeather::kMaxForecastRows; ++i) {
    if (!hasWeatherData || static_cast<int>(i) >= rowCount) {
      continue;
    }
    const auto& snapshot = m_weather->snapshot();
    const bool firstForecastIsToday =
        !snapshot.forecastDays.empty() && snapshot.forecastDays.front().dateIso == todayIso(snapshot.utcOffsetSeconds);
    const std::size_t forecastStart = firstForecastIsToday ? 1 : 0;
    const std::size_t dayIndex = forecastStart + i;
    if (dayIndex >= snapshot.forecastDays.size()) {
      continue;
    }

    const auto& day = snapshot.forecastDays[dayIndex];
    const std::string dayText = weekdayAbbrev(day.dateIso);
    const std::string_view glyphName = WeatherService::glyphForCode(day.weatherCode, true);
    const std::string tempsText = std::format(
        "{} / {}{}", static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMinC))),
        static_cast<int>(std::lround(m_weather->displayTemperature(day.temperatureMaxC))),
        m_weather->displayTemperatureUnit()
    );

    data.push_back({dayText, std::string(glyphName), tempsText});
  }
  const bool changed = data != m_data.forecast;
  m_data.forecast = std::move(data);
  return changed;
}
