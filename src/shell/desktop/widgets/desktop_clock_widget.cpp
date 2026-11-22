#include "shell/desktop/widgets/desktop_clock_widget.h"

#include "clock_widget.h"
#include "time/time_format.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <utility>

namespace {
  struct LocalTimeParts {
    int hour = 0;
    int minute = 0;
    int second = 0;
  };

  LocalTimeParts currentLocalTimeParts(const std::string& tzName) {
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    if (tzName.empty()) {
      const std::time_t timestamp = system_clock::to_time_t(now);
      std::tm localTime{};
#ifdef _WIN32
      localtime_s(&localTime, &timestamp);
#else
      ::tzset();
      localtime_r(&timestamp, &localTime);
#endif
      return {
          .hour = localTime.tm_hour,
          .minute = localTime.tm_min,
          .second = localTime.tm_sec,
      };
    }

    const time_zone* tz = nullptr;
    try {
      tz = locate_zone(tzName);
    } catch (...) {
    }

    if (tz == nullptr) {
      return currentLocalTimeParts("");
    }

    const auto local = tz->to_local(now);
    const auto localDays = floor<days>(local);
    hh_mm_ss time{floor<seconds>(local - localDays)};
    return {
        .hour = static_cast<int>(time.hours().count()),
        .minute = static_cast<int>(time.minutes().count()),
        .second = static_cast<int>(time.seconds().count()),
    };
  }

} // namespace

DesktopClockWidget::Style DesktopClockWidget::styleFromSetting(std::string_view value) {
  if (value == "analog") {
    return Style::Analog;
  }
  return Style::Digital;
}

DesktopClockWidget::DesktopClockWidget(Options options)
    : m_style(options.style), m_format(std::move(options.format)), m_color(options.color), m_shadow(options.shadow),
      m_showCircle(options.showCircle), m_showSeconds(options.showSeconds), m_timezone(std::move(options.timezone)),
      m_centerText(options.centerText),
      m_showsSeconds((m_style == Style::Analog && m_showSeconds) || formatShowsSeconds(m_format)),
      m_fontSize(options.fontSize), m_fontWeight(options.fontWeight), m_tightText(options.tightText) {}

void DesktopClockWidget::create() {
  auto clock = std::make_unique<DesktopClock>();
  m_clock = clock.get();
  clock->setStyle(m_style == Style::Analog ? DesktopClock::Style::Analog : DesktopClock::Style::Digital);
  clock->setColor(m_color);
  clock->setFontSize(m_fontSize);
  clock->setFontWeight(m_fontWeight);
  clock->setTightText(m_tightText);
  clock->setShadow(m_shadow);
  clock->setCenterText(m_centerText);
  clock->setShowCircle(m_showCircle);
  clock->setShowSeconds(m_showSeconds);
  clock->setContentScale(contentScale());
  setRoot(std::move(clock));
  updateAnalogTime();
}

bool DesktopClockWidget::wantsSecondTicks() const { return m_showsSeconds; }

std::string DesktopClockWidget::formatText() const {
  if (!m_timezone.empty()) {
    return formatTimezoneTime(m_format.c_str(), m_timezone);
  }
  return formatLocalTime(m_format.c_str());
}

void DesktopClockWidget::updateAnalogTime() {
  const LocalTimeParts time = currentLocalTimeParts(m_timezone);
  m_clock->setTime(
      std::chrono::hours{time.hour} + std::chrono::minutes{time.minute} + std::chrono::seconds{time.second}
  );
}

bool DesktopClockWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "font_size") {
    if (const auto* size = std::get_if<double>(&value)) {
      m_fontSize = static_cast<float>(*size);
      if (m_clock) {
        m_clock->setFontSize(m_fontSize);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "font_weight") {
    if (const auto* weight = std::get_if<std::int64_t>(&value)) {
      m_fontWeight = static_cast<FontWeight>(*weight);
      if (m_clock) {
        m_clock->setFontWeight(m_fontWeight);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "tight_text") {
    if (const auto* tight = std::get_if<bool>(&value)) {
      m_tightText = *tight;
      if (m_clock) {
        m_clock->setTightText(m_tightText);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "clock_style") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_style = styleFromSetting(*v);
      m_showsSeconds = (m_style == Style::Analog && m_showSeconds) || formatShowsSeconds(m_format);
      if (m_clock != nullptr) {
        m_clock->setStyle(m_style == Style::Analog ? DesktopClock::Style::Analog : DesktopClock::Style::Digital);
      }
      requestLayout();
      (void)allSettings;
      (void)renderer;
      return true;
    }
    return false;
  }
  if (key == "timezone") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_timezone = *v;
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "format") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_format = *v;
      m_showsSeconds = (m_style == Style::Analog && m_showSeconds) || formatShowsSeconds(m_format);
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value); v != nullptr) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_clock != nullptr) {
        m_clock->setColor(m_color);
      }
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      if (m_clock != nullptr) {
        m_clock->setShadow(m_shadow);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "circle") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showCircle = *v;
      if (m_clock != nullptr) {
        m_clock->setShowCircle(m_showCircle);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "show_seconds") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showSeconds = *v;
      m_showsSeconds = (m_style == Style::Analog && m_showSeconds) || formatShowsSeconds(m_format);
      if (m_clock != nullptr) {
        m_clock->setShowSeconds(m_showSeconds);
      }
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "center_text") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_centerText = *v;
      if (m_clock != nullptr) {
        m_clock->setCenterText(m_centerText);
      }
      requestLayout();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopClockWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_clock != nullptr) {
    m_clock->setFontFamily(family);
  }
}

void DesktopClockWidget::doLayout(Renderer& renderer) {
  if (m_clock == nullptr) {
    return;
  }
  m_clock->setContentScale(contentScale());
  update(renderer);
  m_clock->layout(renderer);
}

void DesktopClockWidget::doUpdate(Renderer& renderer) {
  if (m_clock == nullptr) {
    return;
  }
  if (m_style == Style::Analog) {
    updateAnalogTime();
    return;
  }
  const float previousWidth = m_clock->width();
  const float previousHeight = m_clock->height();
  m_clock->setContentScale(contentScale());
  m_clock->setText(formatText());
  if (isLayingOut()) {
    return;
  }
  m_clock->layout(renderer);
  // Stable digit ticks update in place; geometry changes need host fitting.
  if (std::abs(m_clock->width() - previousWidth) > 0.5F || std::abs(m_clock->height() - previousHeight) > 0.5F) {
    requestLayout();
  }
}
