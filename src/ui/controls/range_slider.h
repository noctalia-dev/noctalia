#pragma once

#include "ui/controls/flex.h"
#include "ui/style.h"

#include <functional>

class InputArea;
class RectNode;

// Dual-thumb slider expressing a low/high pair on one axis (e.g. activity + critical
// thresholds). The two thumbs cannot cross — the low value is always <= the high value.
class RangeSlider : public Flex {
public:
  RangeSlider();

  void setRange(double minValue, double maxValue);
  void setStep(double step);
  void setLowValue(double value);
  void setHighValue(double value);
  // Sets both endpoints at once (init path) without cross-clamping against stale values.
  void setValues(double low, double high);
  void setEnabled(bool enabled);
  void setTrackHeight(float height);
  void setThumbSize(float size);
  void setControlHeight(float height);
  void setOnLowChanged(std::function<void(double)> callback);
  void setOnHighChanged(std::function<void(double)> callback);
  void setOnDragEnd(std::function<void()> callback);

  [[nodiscard]] double lowValue() const noexcept { return m_low; }
  [[nodiscard]] double highValue() const noexcept { return m_high; }
  [[nodiscard]] double minValue() const noexcept { return m_min; }
  [[nodiscard]] double maxValue() const noexcept { return m_max; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool dragging() const noexcept;

private:
  enum class ActiveThumb : std::uint8_t { None, Low, High };

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void updateFromLocalX(float x);
  void updateGeometry();
  void applyVisualState();
  [[nodiscard]] float normalized(double value) const noexcept;
  [[nodiscard]] double snapped(double value) const noexcept;
  [[nodiscard]] float thumbCenterX(double value) const noexcept;

  RectNode* m_track = nullptr;
  RectNode* m_fill = nullptr;
  RectNode* m_lowThumb = nullptr;
  RectNode* m_highThumb = nullptr;
  InputArea* m_inputArea = nullptr;

  std::function<void(double)> m_onLowChanged;
  std::function<void(double)> m_onHighChanged;
  std::function<void()> m_onDragEnd;

  double m_min = 0.0;
  double m_max = 100.0;
  double m_step = 1.0;
  double m_low = 25.0;
  double m_high = 75.0;
  ActiveThumb m_activeThumb = ActiveThumb::None;
  bool m_enabled = true;
  float m_trackHeight = Style::sliderTrackHeight;
  float m_thumbSizePx = Style::sliderThumbSize;
  float m_controlHeightPx = Style::controlHeight;
};
