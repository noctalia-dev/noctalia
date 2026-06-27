#include "ui/controls/stepper.h"

#include "core/key_symbols.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kDefaultMinWidth = 100.0f;
  // Matches Input's internal text viewport inset so the layout minimum reserves
  // the visible text box, not just ink.
  constexpr float kInputTextInnerInset = 3.0f;
  constexpr std::chrono::milliseconds kStepRepeatDelay{350};
  constexpr std::chrono::milliseconds kStepRepeatInterval{70};

  float valueInputHorizontalPadding(float scale) { return Style::spaceSm * scale; }

  std::unique_ptr<Separator> makeStepperSeparator(float scale) {
    auto sep = std::make_unique<Separator>();
    sep->setOrientation(SeparatorOrientation::VerticalRule);
    sep->setThickness(std::max(1.0f, Style::borderWidth * scale));
    sep->setColor(colorSpecFromRole(ColorRole::Outline));
    sep->setFlexGrow(0.0f);
    return sep;
  }

  std::string trimAscii(std::string_view s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) {
      ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) {
      --b;
    }
    return std::string(s.substr(a, b - a));
  }

} // namespace

Stepper::Stepper() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setJustify(FlexJustify::Start);
  setGap(0.0f);
  setPadding(0.0f);
  setMinWidth(kDefaultMinWidth);
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, m_surfaceOpacity));
  clearBorder();
  setRadius(Style::scaledRadiusMd());

  auto makeStepButton = [this](bool increment) -> std::unique_ptr<Button> {
    auto btn = std::make_unique<Button>();
    btn->setVariant(ButtonVariant::Tab);
    btn->setGlyph(increment ? "plus" : "minus");
    btn->setGlyphSize(Style::fontSizeBody);
    btn->setMinHeight(Style::controlHeight);
    btn->setPadding(Style::spaceXs, Style::spaceMd);
    btn->setContentAlign(ButtonContentAlign::Center);
    btn->setFlexGrow(0.0f);
    btn->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    btn->setOnPress([this, directionSign = increment ? 1 : -1](float /*localX*/, float /*localY*/, bool pressed) {
      if (pressed) {
        beginStepRepeat(directionSign);
      } else {
        stopStepRepeat();
      }
    });
    btn->setOnLeave([this]() { stopStepRepeat(); });
    btn->setTabStop(false);
    return btn;
  };

  {
    auto dec = makeStepButton(false);
    m_decrement = dec.get();
    addChild(std::move(dec));
  }

  {
    auto sep = makeStepperSeparator(m_scale);
    m_separatorBeforeValue = sep.get();
    addChild(std::move(sep));
  }

  {
    auto track = std::make_unique<Flex>();
    track->setDirection(FlexDirection::Horizontal);
    track->setAlign(FlexAlign::Stretch);
    track->setJustify(FlexJustify::Center);
    track->setGap(0.0f);
    track->setPadding(0.0f);
    track->setFlexGrow(1.0f);
    track->setMinHeight(Style::controlHeight);
    track->clearFill();
    track->clearBorder();
    track->setRadii(Radii{});
    m_valueTrack = track.get();

    auto field = std::make_unique<Input>();
    field->setFrameVisible(false);
    field->setTextAlign(TextAlign::Center);
    field->setFontSize(Style::fontSizeBody);
    field->setControlHeight(Style::controlHeight);
    field->setHorizontalPadding(valueInputHorizontalPadding(m_scale));
    field->setFlexGrow(1.0f);
    field->setOnSubmit([this](const std::string& /*text*/) { commitValueField(); });
    field->setOnFocusLoss([this]() { commitValueField(); });
    field->setOnKeyEvent([this](std::uint32_t sym, std::uint32_t mod) { return swallowNonNumericKey(sym, mod); });
    m_valueInput = field.get();
    track->addChild(std::move(field));
    addChild(std::move(track));
  }

  {
    auto sep = makeStepperSeparator(m_scale);
    m_separatorAfterValue = sep.get();
    addChild(std::move(sep));
  }

  {
    auto inc = makeStepButton(true);
    m_increment = inc.get();
    addChild(std::move(inc));
  }

  syncValueField();
  refreshButtons();
  refreshSegmentStyle();
}

void Stepper::setRange(int minValue, int maxValue) {
  if (maxValue < minValue) {
    std::swap(minValue, maxValue);
  }
  if (m_min == minValue && m_max == maxValue) {
    return;
  }
  m_min = minValue;
  m_max = maxValue;
  setValue(m_value);
  markLayoutDirty();
}

void Stepper::setStep(int step) {
  const int next = std::max(1, step);
  if (m_step == next) {
    return;
  }
  m_step = next;
  setValue(m_value);
}

void Stepper::setValue(int value) {
  const int next = std::clamp(value, m_min, m_max);
  if (next == m_value) {
    syncValueField();
    refreshButtons();
    return;
  }
  m_value = next;
  syncValueField();
  refreshButtons();
  if (m_onValueChanged) {
    m_onValueChanged(m_value);
  }
  markLayoutDirty();
}

void Stepper::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  refreshSegmentStyle();
}

void Stepper::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (!m_enabled) {
    stopStepRepeat();
  }
  refreshButtons();
  if (m_valueInput != nullptr) {
    m_valueInput->inputArea()->setEnabled(enabled);
    m_valueInput->inputArea()->setFocusable(enabled);
  }
  markPaintDirty();
}

void Stepper::setOnValueChanged(std::function<void(int)> callback) { m_onValueChanged = std::move(callback); }

void Stepper::setOnValueCommitted(std::function<void(int)> callback) { m_onValueCommitted = std::move(callback); }

void Stepper::setValueSuffix(std::string suffix) {
  if (m_valueSuffix == suffix) {
    return;
  }
  m_valueSuffix = std::move(suffix);
  syncValueField();
  markLayoutDirty();
}

void Stepper::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  setGap(0.0f);
  setPadding(0.0f);
  setMinWidth(kDefaultMinWidth * m_scale);
  clearBorder();
  if (m_valueTrack != nullptr) {
    m_valueTrack->setMinHeight(Style::controlHeight * m_scale);
  }
  if (m_valueInput != nullptr) {
    m_valueInput->setFontSize(Style::fontSizeBody * m_scale);
    m_valueInput->setControlHeight(Style::controlHeight * m_scale);
    m_valueInput->setHorizontalPadding(valueInputHorizontalPadding(m_scale));
  }
  if (m_decrement != nullptr) {
    m_decrement->setGlyphSize(Style::fontSizeBody * m_scale);
    m_decrement->setMinHeight(Style::controlHeight * m_scale);
    m_decrement->setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
  }
  if (m_increment != nullptr) {
    m_increment->setGlyphSize(Style::fontSizeBody * m_scale);
    m_increment->setMinHeight(Style::controlHeight * m_scale);
    m_increment->setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
  }
  refreshSegmentStyle();
  markLayoutDirty();
}

void Stepper::syncValueFieldMinWidth(Renderer& renderer) {
  if (m_valueInput == nullptr) {
    return;
  }
  const float fs = Style::fontSizeBody * m_scale;
  const std::string minText = std::to_string(m_min) + m_valueSuffix;
  const std::string maxText = std::to_string(m_max) + m_valueSuffix;
  const float wMin = renderer.measureText(minText, fs, FontWeight::Normal).width;
  const float wMax = renderer.measureText(maxText, fs, FontWeight::Normal).width;
  const float textInset = valueInputHorizontalPadding(m_scale) + kInputTextInnerInset;
  m_valueInput->setMinLayoutWidth(std::max(wMin, wMax) + textInset * 2.0f);
}

LayoutSize Stepper::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  syncValueFieldMinWidth(renderer);
  return Flex::doMeasure(renderer, constraints);
}

void Stepper::doLayout(Renderer& renderer) {
  syncValueFieldMinWidth(renderer);
  Flex::doLayout(renderer);
  if (m_decrement != nullptr) {
    m_decrement->updateInputArea();
  }
  if (m_increment != nullptr) {
    m_increment->updateInputArea();
  }
}

void Stepper::beginStepRepeat(int directionSign) {
  stopStepRepeat();
  if (!m_enabled || directionSign == 0) {
    return;
  }
  m_repeatDirection = directionSign;
  m_repeatStartValue = m_value;
  repeatStep();
  if (m_repeatDirection == 0) {
    return;
  }
  m_repeatDelayTimer.start(kStepRepeatDelay, [this]() {
    if (m_repeatDirection == 0) {
      return;
    }
    m_repeatTimer.startRepeating(kStepRepeatInterval, [this]() { repeatStep(); });
  });
}

void Stepper::repeatStep() {
  if (!m_enabled || m_repeatDirection == 0) {
    stopStepRepeat();
    return;
  }
  if ((m_repeatDirection < 0 && m_value <= m_min) || (m_repeatDirection > 0 && m_value >= m_max)) {
    stopStepRepeat();
    return;
  }
  stepBy(m_repeatDirection);
  if ((m_repeatDirection < 0 && m_value <= m_min) || (m_repeatDirection > 0 && m_value >= m_max)) {
    stopStepRepeat();
  }
}

void Stepper::stopStepRepeat() {
  const bool changed = m_repeatDirection != 0 && m_value != m_repeatStartValue;
  m_repeatDelayTimer.stop();
  m_repeatTimer.stop();
  m_repeatDirection = 0;
  if (changed) {
    emitValueCommitted();
  }
}

void Stepper::emitValueCommitted() {
  if (m_onValueCommitted) {
    m_onValueCommitted(m_value);
  }
}

void Stepper::stepBy(int directionSign) {
  if (!m_enabled || directionSign == 0) {
    return;
  }
  const long delta = static_cast<long>(m_step) * static_cast<long>(directionSign);
  const long nextLong = static_cast<long>(m_value) + delta;
  const int next = static_cast<int>(std::clamp(nextLong, static_cast<long>(m_min), static_cast<long>(m_max)));
  setValue(next);
}

void Stepper::syncValueField() {
  if (m_valueInput != nullptr) {
    m_valueInput->setValue(std::to_string(m_value) + m_valueSuffix);
  }
}

void Stepper::commitValueField() {
  if (m_valueInput == nullptr || !m_enabled) {
    return;
  }
  const int previous = m_value;
  std::string t = trimAscii(m_valueInput->value());
  if (t.empty()) {
    syncValueField();
    return;
  }
  while (!m_valueSuffix.empty() && t.size() >= m_valueSuffix.size() && t.ends_with(m_valueSuffix)) {
    t.resize(t.size() - m_valueSuffix.size());
    t = trimAscii(t);
  }
  if (t.empty()) {
    syncValueField();
    return;
  }
  try {
    std::size_t idx = 0;
    const long v = std::stol(t, &idx, 10);
    if (idx != t.size()) {
      syncValueField();
      return;
    }
    setValue(static_cast<int>(v));
    if (m_value != previous) {
      emitValueCommitted();
    }
  } catch (const std::logic_error&) {
    syncValueField();
  }
}

bool Stepper::swallowNonNumericKey(std::uint32_t sym, std::uint32_t modifiers) {
  (void)modifiers;
  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    return false;
  }
  if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9) {
    return false;
  }
  if (m_min < 0) {
    if (sym == XKB_KEY_minus || sym == XKB_KEY_KP_Subtract) {
      return false;
    }
  }
  if (sym == XKB_KEY_plus || sym == XKB_KEY_KP_Add) {
    return true;
  }
  if ((sym >= XKB_KEY_a && sym <= XKB_KEY_z) || (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)) {
    return true;
  }
  if (KeySymbol::isSpace(sym)) {
    return true;
  }
  if (sym == XKB_KEY_period || sym == XKB_KEY_comma) {
    return true;
  }
  if (sym == XKB_KEY_KP_Decimal || sym == XKB_KEY_KP_Separator) {
    return true;
  }
  return false;
}

void Stepper::refreshButtons() {
  if (m_decrement != nullptr) {
    m_decrement->setEnabled(m_enabled && m_value > m_min);
  }
  if (m_increment != nullptr) {
    m_increment->setEnabled(m_enabled && m_value < m_max);
  }
}

void Stepper::refreshSegmentStyle() {
  const float r = Style::scaledRadiusMd(m_scale);
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, m_surfaceOpacity));
  clearBorder();
  setRadius(r);
  if (m_decrement != nullptr) {
    m_decrement->setVariant(ButtonVariant::Tab);
    m_decrement->setRadii({r, 0.0f, 0.0f, r});
  }
  if (m_increment != nullptr) {
    m_increment->setVariant(ButtonVariant::Tab);
    m_increment->setRadii({0.0f, r, r, 0.0f});
  }
  if (m_valueTrack != nullptr) {
    m_valueTrack->setRadii(Radii{});
    m_valueTrack->clearFill();
  }
  const float ruleW = std::max(1.0f, Style::borderWidth * m_scale);
  if (m_separatorBeforeValue != nullptr) {
    m_separatorBeforeValue->setThickness(ruleW);
  }
  if (m_separatorAfterValue != nullptr) {
    m_separatorAfterValue->setThickness(ruleW);
  }
}
