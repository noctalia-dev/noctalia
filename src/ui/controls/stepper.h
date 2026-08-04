#pragma once

#include "core/timer_manager.h"
#include "ui/controls/flex.h"

#include <cstdint>
#include <functional>
#include <string>

class Button;
class Input;
class Separator;
class Renderer;

// Horizontal numeric stepper styled like Segmented (shared SurfaceVariant track, Tab-style ends + center).
class Stepper : public Flex {
public:
  Stepper();

  void setRange(int minValue, int maxValue);
  void setStep(int step);
  void setValue(int value);
  void setEnabled(bool enabled);
  void setSurfaceOpacity(float opacity);
  void setOnValueChanged(std::function<void(int)> callback);
  void setOnValueCommitted(std::function<void(int)> callback);
  void setScale(float scale);
  /// When non-empty, appended to the value field for display (e.g. "s" → "5s"). Stripped before parsing on commit.
  void setValueSuffix(std::string suffix);

  [[nodiscard]] int value() const noexcept { return m_value; }
  [[nodiscard]] int minValue() const noexcept { return m_min; }
  [[nodiscard]] int maxValue() const noexcept { return m_max; }
  [[nodiscard]] int step() const noexcept { return m_step; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  [[nodiscard]] Input* valueField() const noexcept { return m_valueInput; }
  [[nodiscard]] Button* decrementButton() const noexcept { return m_decrement; }
  [[nodiscard]] Button* incrementButton() const noexcept { return m_increment; }

private:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;
  void syncValueFieldMinWidth(Renderer& renderer);
  void beginStepRepeat(int directionSign);
  void repeatStep();
  void stopStepRepeat();
  void emitValueCommitted();
  void stepBy(int directionSign);
  void syncValueField();
  void commitValueField();
  bool swallowNonNumericKey(std::uint32_t sym, std::uint32_t modifiers);
  void refreshButtons();
  void refreshSegmentStyle();

  Button* m_decrement = nullptr;
  Separator* m_separatorBeforeValue = nullptr;
  Flex* m_valueTrack = nullptr;
  Separator* m_separatorAfterValue = nullptr;
  Button* m_increment = nullptr;
  Input* m_valueInput = nullptr;

  std::function<void(int)> m_onValueChanged;
  std::function<void(int)> m_onValueCommitted;

  int m_min = 0;
  int m_max = 100;
  int m_step = 1;
  int m_value = 0;
  float m_surfaceOpacity = 1.0F;
  bool m_enabled = true;
  float m_scale = 1.0F;
  int m_repeatDirection = 0;
  int m_repeatStartValue = 0;
  Timer m_repeatDelayTimer;
  Timer m_repeatTimer;
  std::string m_valueSuffix;
};
