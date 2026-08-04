#include "shell/desktop/widgets/desktop_clock_widget.h"

#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <numbers>
#include <utility>

namespace {

  bool formatShowsSeconds(const std::string& format) {
    return format.contains("%S") || format.contains("%T") || format.contains("%X");
  }

  float clockFontSize(float contentScale) { return Style::fontSizeBody * 4.0F * contentScale; }

  float analogClockSize(float contentScale) { return Style::fontSizeBody * 5.25F * contentScale; }

  struct AnalogDialMetrics {
    float center = 0.0F;
    float scale = 1.0F;
    float dialRadius = 0.0F;
    float borderWidth = 0.0F;
    float faceSize = 0.0F;
    float faceOffset = 0.0F;
    float minuteLength = 0.0F;
    float minuteWidth = 0.0F;
    float hourLength = 0.0F;
    float hourWidth = 0.0F;
  };

  [[nodiscard]] AnalogDialMetrics analogDialMetrics(float size, float scale) {
    const float center = size * 0.5F;
    const float borderWidth = std::max(1.5F, 2.0F * scale);
    const float hourLength = std::max(4.0F, 5.5F * scale);
    const float hourWidth = std::max(1.75F, 2.25F * scale);
    const float edgePadding = std::max(3.0F, 3.5F * scale) + hourWidth * 0.5F;
    const float dialRadius = std::max(1.0F, center - edgePadding - borderWidth * 0.5F);
    const float faceSize = dialRadius * 2.0F;
    return {
        .center = center,
        .scale = scale,
        .dialRadius = dialRadius,
        .borderWidth = borderWidth,
        .faceSize = faceSize,
        .faceOffset = center - dialRadius,
        .minuteLength = std::max(2.5F, 3.0F * scale),
        .minuteWidth = std::max(1.0F, 1.25F * scale),
        .hourLength = hourLength,
        .hourWidth = hourWidth,
    };
  }

  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;
  constexpr float kHourHandReach = 0.54F;
  constexpr float kMinuteHandReach = 0.90F;
  constexpr float kSecondHandReach = 0.84F;

  [[nodiscard]] float degreesToRadians(float degrees) { return degrees * (std::numbers::pi_v<float> / 180.0F); }

  struct LocalTimeParts {
    int hour = 0;
    int minute = 0;
    int second = 0;
  };

  [[nodiscard]] LocalTimeParts currentLocalTimeParts(const std::string& tzName) {
    using namespace std::chrono;
    const auto now = floor<seconds>(system_clock::now());
    if (tzName.empty()) {
      const std::time_t timestamp = system_clock::to_time_t(now);
      std::tm localTime{};
#if defined(_WIN32)
      localtime_s(&localTime, &timestamp);
#else
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

  [[nodiscard]] Color resolvedColor(const ColorSpec& spec) { return resolveColorSpec(spec); }

  RectNode* addHand(Node& pivot, float width, float length, const Color& color) {
    auto hand = std::make_unique<RectNode>();
    hand->setSize(width, length);
    hand->setPosition(-width * 0.5F, -length);
    RoundedRectStyle style;
    style.fill = color;
    style.radius = width * 0.5F;
    hand->setStyle(style);
    return static_cast<RectNode*>(pivot.addChild(std::move(hand)));
  }

  Node* addHandPivot(Node& parent, float center, float width, float length, const Color& color) {
    auto pivot = ui::node({});
    pivot->setPosition(center, center);
    pivot->setSize(0.0F, 0.0F);
    pivot->setParticipatesInLayout(false);
    auto* pivotPtr = static_cast<Node*>(parent.addChild(std::move(pivot)));
    (void)addHand(*pivotPtr, width, length, color);
    return pivotPtr;
  }

  void addTickMark(
      Node& parent, float center, float angleRad, float width, float length, float dialRadius, const Color& color
  ) {
    auto pivot = ui::node({});
    pivot->setPosition(center, center);
    pivot->setRotation(angleRad);
    pivot->setParticipatesInLayout(false);
    auto* pivotPtr = parent.addChild(std::move(pivot));

    auto mark = std::make_unique<RectNode>();
    mark->setSize(width, length);
    mark->setPosition(-width * 0.5F, -dialRadius);
    RoundedRectStyle style;
    style.fill = color;
    style.radius = width * 0.5F;
    mark->setStyle(style);
    pivotPtr->addChild(std::move(mark));
  }

  void buildAnalogTicks(Node& ticksRoot, const AnalogDialMetrics& metrics, const Color& color) {
    for (int minute = 0; minute < 60; ++minute) {
      const bool hourMark = (minute % 5) == 0;
      const float angle = degreesToRadians(static_cast<float>(minute) * 6.0F);
      addTickMark(
          ticksRoot, metrics.center, angle, hourMark ? metrics.hourWidth : metrics.minuteWidth,
          hourMark ? metrics.hourLength : metrics.minuteLength, metrics.dialRadius, color
      );
    }
  }

  void layoutAnalogTicks(Node& ticksRoot, const AnalogDialMetrics& metrics, const Color& color) {
    for (std::size_t minute = 0; minute < ticksRoot.children().size(); ++minute) {
      const bool hourMark = (minute % 5) == 0;
      const float width = hourMark ? metrics.hourWidth : metrics.minuteWidth;
      const float length = hourMark ? metrics.hourLength : metrics.minuteLength;
      const float angle = degreesToRadians(static_cast<float>(minute) * 6.0F);

      auto* pivot = ticksRoot.children()[minute].get();
      pivot->setPosition(metrics.center, metrics.center);
      pivot->setRotation(angle);

      auto* mark = static_cast<RectNode*>(pivot->children().front().get());
      mark->setSize(width, length);
      mark->setPosition(-width * 0.5F, -metrics.dialRadius);
      RoundedRectStyle style = mark->style();
      style.fill = color;
      style.radius = width * 0.5F;
      mark->setStyle(style);
    }
  }

  void layoutAnalogFace(RectNode& face, const AnalogDialMetrics& metrics, const Color& borderColor) {
    face.setSize(metrics.faceSize, metrics.faceSize);
    face.setPosition(metrics.faceOffset, metrics.faceOffset);
    RoundedRectStyle faceStyle = face.style();
    faceStyle.border = borderColor;
    faceStyle.borderWidth = metrics.borderWidth;
    faceStyle.radius = metrics.dialRadius;
    face.setStyle(faceStyle);
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
      m_showCircle(options.showCircle), m_timezone(std::move(options.timezone)), m_centerText(options.centerText),
      m_showsSeconds(m_style == Style::Analog || formatShowsSeconds(m_format)) {}

void DesktopClockWidget::create() {
  auto rootNode = ui::node({});

  auto digitalRoot = ui::node({});
  m_digitalRoot = digitalRoot.get();
  auto label = ui::label({
      .out = &m_label,
      .fontSize = clockFontSize(contentScale()),
      .fontWeight = FontWeight::Bold,
      .color = m_color,
  });
  m_digitalRoot->addChild(std::move(label));
  rootNode->addChild(std::move(digitalRoot));

  auto analogRoot = ui::node({});
  m_analogRoot = analogRoot.get();
  m_analogRoot->setVisible(m_style == Style::Analog);

  const float scale = contentScale();
  const float size = analogClockSize(scale);
  m_analogRoot->setSize(size, size);

  const Color handColor = resolvedColor(m_color);
  const Color secondColor = Color(handColor.r, handColor.g, handColor.b, handColor.a * 0.72F);
  const AnalogDialMetrics metrics = analogDialMetrics(size, scale);

  auto face = std::make_unique<RectNode>();
  m_face = face.get();
  RoundedRectStyle faceStyle;
  faceStyle.fill = Color(0.0F, 0.0F, 0.0F, 0.0F);
  faceStyle.border = handColor;
  m_face->setStyle(faceStyle);
  m_analogRoot->addChild(std::move(face));

  auto ticksRoot = ui::node({});
  m_ticksRoot = ticksRoot.get();
  m_ticksRoot->setParticipatesInLayout(false);
  buildAnalogTicks(*m_ticksRoot, metrics, handColor);
  m_analogRoot->addChild(std::move(ticksRoot));

  const float hourWidth = std::max(2.2F, 2.65F * scale);
  const float minuteWidth = std::max(1.75F, 2.0F * scale);
  const float secondWidth = std::max(1.0F, 1.25F * scale);
  m_hourPivot = addHandPivot(*m_analogRoot, metrics.center, hourWidth, metrics.dialRadius * kHourHandReach, handColor);
  m_minutePivot =
      addHandPivot(*m_analogRoot, metrics.center, minuteWidth, metrics.dialRadius * kMinuteHandReach, handColor);
  m_secondPivot =
      addHandPivot(*m_analogRoot, metrics.center, secondWidth, metrics.dialRadius * kSecondHandReach, secondColor);

  auto hub = std::make_unique<RectNode>();
  m_hub = hub.get();
  const float hubSize = std::max(4.0F, 5.0F * scale);
  hub->setSize(hubSize, hubSize);
  hub->setPosition(metrics.center - hubSize * 0.5F, metrics.center - hubSize * 0.5F);
  RoundedRectStyle hubStyle;
  hubStyle.fill = handColor;
  hubStyle.radius = hubSize * 0.5F;
  hub->setStyle(hubStyle);
  m_analogRoot->addChild(std::move(hub));

  rootNode->addChild(std::move(analogRoot));
  setRoot(std::move(rootNode));
  syncDigitalTextAlign();
  syncStyleVisibility();
  syncCircleVisibility();
  applyShadow();
  syncAnalogColors();
  updateAnalogHands();
}

bool DesktopClockWidget::wantsSecondTicks() const { return m_showsSeconds; }

std::string DesktopClockWidget::formatText() const {
  if (!m_timezone.empty()) {
    return formatTimezoneTime(m_format.c_str(), m_timezone);
  }
  return formatLocalTime(m_format.c_str());
}

void DesktopClockWidget::syncStyleVisibility() {
  if (m_digitalRoot != nullptr) {
    m_digitalRoot->setVisible(m_style == Style::Digital);
  }
  if (m_analogRoot != nullptr) {
    m_analogRoot->setVisible(m_style == Style::Analog);
  }
}

void DesktopClockWidget::syncCircleVisibility() {
  if (m_face != nullptr) {
    m_face->setVisible(m_showCircle);
  }
  if (m_ticksRoot != nullptr) {
    m_ticksRoot->setVisible(m_showCircle);
  }
}

void DesktopClockWidget::syncDigitalTextAlign() {
  if (m_label == nullptr) {
    return;
  }
  m_label->setTextAlign(m_centerText ? TextAlign::Center : TextAlign::Start);
}

void DesktopClockWidget::syncAnalogColors() {
  if (m_face == nullptr || m_analogRoot == nullptr) {
    return;
  }

  const Color handColor = resolvedColor(m_color);
  const Color secondColor = Color(handColor.r, handColor.g, handColor.b, handColor.a * 0.72F);
  const float scale = contentScale();
  const AnalogDialMetrics metrics = analogDialMetrics(m_analogRoot->width(), scale);

  RoundedRectStyle faceStyle = m_face->style();
  faceStyle.border = handColor;
  m_face->setStyle(faceStyle);
  layoutAnalogFace(*m_face, metrics, handColor);

  const auto applyHandColor = [](Node* pivot, const Color& color) {
    if (pivot == nullptr || pivot->children().empty()) {
      return;
    }
    auto* hand = static_cast<RectNode*>(pivot->children().front().get());
    RoundedRectStyle style = hand->style();
    style.fill = color;
    hand->setStyle(style);
  };
  applyHandColor(m_hourPivot, handColor);
  applyHandColor(m_minutePivot, handColor);
  applyHandColor(m_secondPivot, secondColor);

  if (m_ticksRoot != nullptr && m_showCircle) {
    layoutAnalogTicks(*m_ticksRoot, metrics, handColor);
  }
}

void DesktopClockWidget::layoutAnalog(Renderer& /*renderer*/, float size) {
  if (m_analogRoot == nullptr || m_face == nullptr) {
    return;
  }

  m_analogRoot->setSize(size, size);

  const float scale = contentScale();
  const AnalogDialMetrics metrics = analogDialMetrics(size, scale);
  const Color handColor = resolvedColor(m_color);

  layoutAnalogFace(*m_face, metrics, handColor);

  if (m_hourPivot != nullptr) {
    m_hourPivot->setPosition(metrics.center, metrics.center);
  }
  if (m_minutePivot != nullptr) {
    m_minutePivot->setPosition(metrics.center, metrics.center);
  }
  if (m_secondPivot != nullptr) {
    m_secondPivot->setPosition(metrics.center, metrics.center);
  }

  const float hourWidth = std::max(2.2F, 2.65F * scale);
  const float minuteWidth = std::max(1.75F, 2.0F * scale);
  const float secondWidth = std::max(1.0F, 1.25F * scale);
  const auto resizeHand = [](Node* pivot, float width, float length) {
    if (pivot == nullptr || pivot->children().empty()) {
      return;
    }
    auto* hand = static_cast<RectNode*>(pivot->children().front().get());
    hand->setSize(width, length);
    hand->setPosition(-width * 0.5F, -length);
    RoundedRectStyle style = hand->style();
    style.radius = width * 0.5F;
    hand->setStyle(style);
  };
  resizeHand(m_hourPivot, hourWidth, metrics.dialRadius * kHourHandReach);
  resizeHand(m_minutePivot, minuteWidth, metrics.dialRadius * kMinuteHandReach);
  resizeHand(m_secondPivot, secondWidth, metrics.dialRadius * kSecondHandReach);

  if (m_hub != nullptr) {
    const float hubSize = std::max(4.0F, 5.0F * scale);
    m_hub->setSize(hubSize, hubSize);
    m_hub->setPosition(metrics.center - hubSize * 0.5F, metrics.center - hubSize * 0.5F);
    RoundedRectStyle hubStyle = m_hub->style();
    hubStyle.fill = handColor;
    hubStyle.radius = hubSize * 0.5F;
    m_hub->setStyle(hubStyle);
  }

  if (m_ticksRoot != nullptr && m_showCircle) {
    layoutAnalogTicks(*m_ticksRoot, metrics, handColor);
  }
}

void DesktopClockWidget::layoutDigital(Renderer& renderer) {
  if (m_label == nullptr) {
    return;
  }

  m_label->setFontSize(clockFontSize(contentScale()));
  applyShadow();
  update(renderer);
  m_label->measure(renderer);
  // Start-aligned mode shifts the label right so proportional digits stay visually stable; centered
  // mode relies on TextAlign::Center inside the reserved widest-digit width.
  m_label->setPosition(m_centerText ? 0.0F : m_digitOffsetX, 0.0F);
  if (m_digitalRoot != nullptr) {
    m_digitalRoot->setSize(m_label->width() + m_digitOffsetX, m_label->height());
  }
  if (root() != nullptr && m_digitalRoot != nullptr) {
    root()->setSize(m_digitalRoot->width(), m_digitalRoot->height());
  }
}

void DesktopClockWidget::updateAnalogHands() {
  const LocalTimeParts time = currentLocalTimeParts(m_timezone);
  if (time.hour == m_lastHour && time.minute == m_lastMinute && time.second == m_lastSecond) {
    return;
  }

  m_lastHour = time.hour;
  m_lastMinute = time.minute;
  m_lastSecond = time.second;

  const float hourAngle = degreesToRadians(
      (static_cast<float>(time.hour % 12)
       + static_cast<float>(time.minute) / 60.0F
       + static_cast<float>(time.second) / 3600.0F)
      * 30.0F
  );
  const float minuteAngle =
      degreesToRadians((static_cast<float>(time.minute) + static_cast<float>(time.second) / 60.0F) * 6.0F);
  const float secondAngle = degreesToRadians(static_cast<float>(time.second) * 6.0F);

  if (m_hourPivot != nullptr) {
    m_hourPivot->setRotation(hourAngle);
  }
  if (m_minutePivot != nullptr) {
    m_minutePivot->setRotation(minuteAngle);
  }
  if (m_secondPivot != nullptr) {
    m_secondPivot->setRotation(secondAngle);
  }
}

bool DesktopClockWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "clock_style") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_style = styleFromSetting(*v);
      m_showsSeconds = m_style == Style::Analog || formatShowsSeconds(m_format);
      syncStyleVisibility();
      syncCircleVisibility();
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
      m_lastText.clear();
      m_lastHour = m_lastMinute = m_lastSecond = -1;
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "format") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_format = *v;
      m_showsSeconds = m_style == Style::Analog || formatShowsSeconds(m_format);
      m_lastText.clear();
      requestUpdate();
      return true;
    }
    return false;
  }
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value); v != nullptr) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_label != nullptr) {
        m_label->setColor(m_color);
      }
      syncAnalogColors();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      syncAnalogColors();
      requestRedraw();
      return true;
    }
    return false;
  }
  if (key == "circle") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_showCircle = *v;
      syncCircleVisibility();
      syncAnalogColors();
      requestLayout();
      return true;
    }
    return false;
  }
  if (key == "center_text") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_centerText = *v;
      syncDigitalTextAlign();
      m_stableSample.clear();
      requestLayout();
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopClockWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_label != nullptr) {
    m_label->setFontFamily(family);
  }
}

void DesktopClockWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr) {
    return;
  }

  if (m_style == Style::Analog) {
    layoutAnalog(renderer, analogClockSize(contentScale()));
    if (m_analogRoot != nullptr && root() != nullptr) {
      root()->setSize(m_analogRoot->width(), m_analogRoot->height());
    }
    return;
  }

  layoutDigital(renderer);
}

void DesktopClockWidget::updateStableDigitalWidth(Renderer& renderer, const std::string& text) {
  if (m_label == nullptr) {
    return;
  }

  const float fontSize = clockFontSize(contentScale());

  // Re-pick the widest digit only when the font identity (size or family) changes.
  if (fontSize != m_metricsFontSize || m_fontFamily != m_metricsFontFamily) {
    m_metricsFontSize = fontSize;
    m_metricsFontFamily = m_fontFamily;
    float widest = -1.0F;
    float advanceSum = 0.0F;
    for (char digit = '0'; digit <= '9'; ++digit) {
      const std::string glyph(1, digit);
      const float advance =
          renderer.measureText(glyph, fontSize, FontWeight::Bold, 0.0F, 0, TextAlign::Start, m_fontFamily).width;
      advanceSum += advance;
      if (advance > widest) {
        widest = advance;
        m_widestDigit = digit;
      }
    }
    m_maxDigitAdvance = widest;
    m_meanDigitAdvance = advanceSum / 10.0F;
    m_stableSample.clear(); // force a width recompute below
  }

  // Normalize digits to the widest glyph: the result's width is invariant across
  // seconds (and minutes), so the box keeps a constant size and never reflows.
  std::string sample = text;
  int digitCount = 0;
  for (char& ch : sample) {
    if (ch >= '0' && ch <= '9') {
      ch = m_widestDigit;
      ++digitCount;
    }
  }
  if (sample == m_stableSample) {
    return;
  }
  m_stableSample = sample;

  const float width =
      renderer.measureText(sample, fontSize, FontWeight::Bold, 0.0F, 0, TextAlign::Start, m_fontFamily).width;
  const float offset = m_centerText ? 0.0F : static_cast<float>(digitCount) * (m_maxDigitAdvance - m_meanDigitAdvance);
  if (std::abs(width - m_stableWidth) > 0.5F || std::abs(offset - m_digitOffsetX) > 0.5F) {
    m_stableWidth = width;
    m_digitOffsetX = offset;
    m_label->setMinWidth(m_stableWidth);
    m_label->setMaxWidth(m_stableWidth);
    // Re-arm layout only when the change surfaces outside layout (a non-digit field like the
    // date/AM-PM rolling over on the Update tick). During layout() the new width/offset already
    // apply in this same pass, and its two box-fit passes (base then fitted scale) each measure a
    // different width — re-arming here would loop forever. The nested update() inside doLayout opens
    // an Update phase scope, so guard on isLayingOut() rather than the phase.
    if (!isLayingOut()) {
      requestLayout();
    }
  }
}

void DesktopClockWidget::doUpdate(Renderer& renderer) {
  if (m_style == Style::Analog) {
    updateAnalogHands();
    return;
  }

  if (m_label == nullptr) {
    return;
  }

  m_label->setFontSize(clockFontSize(contentScale()));
  const std::string text = formatText();
  updateStableDigitalWidth(renderer, text);
  if (text == m_lastText) {
    return;
  }

  m_lastText = text;
  m_label->setText(m_lastText);
  m_label->measure(renderer);
}

void DesktopClockWidget::applyShadow() {
  if (m_label == nullptr) {
    return;
  }
  if (m_shadow) {
    const float offset = kShadowOffset * contentScale();
    m_label->setShadow(colorSpecFromRole(ColorRole::Shadow, kShadowAlpha), offset, offset);
  } else {
    m_label->clearShadow();
  }
}
