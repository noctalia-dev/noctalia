#include "ui/controls/segmented.h"

#include "render/core/render_styles.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

Segmented::Segmented() {
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setGap(0.0f);
  applyOuterStyle();
}

std::size_t Segmented::addOption(std::string_view label) { return addOption(label, std::string_view{}); }

std::size_t Segmented::addOption(std::string_view label, std::string_view glyph) {
  const std::size_t index = m_buttons.size();
  if (index > 0) {
    auto sep = makeSegmentSeparator();
    m_separators.push_back(sep.get());
    addChild(std::move(sep));
  }
  auto btn = makeSegmentButton(label, glyph, index);
  Button* raw = btn.get();
  m_buttons.push_back(raw);
  addChild(std::move(btn));
  refreshVariants();
  return index;
}

void Segmented::setSelectedIndex(std::size_t index) {
  if (index >= m_buttons.size() || index == m_selected) {
    return;
  }
  m_selected = index;
  refreshVariants();
  if (m_onChange) {
    m_onChange(index);
  }
}

void Segmented::setFontSize(float size) {
  m_fontSize = size;
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
}

void Segmented::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  applyOuterStyle();
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
  const float ruleW = std::max(1.0f, Style::borderWidth * m_scale);
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      sep->setThickness(ruleW);
    }
  }
  refreshVariants();
  markLayoutDirty();
}

void Segmented::setCompact(bool compact) {
  if (m_compact == compact) {
    return;
  }
  m_compact = compact;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
    }
  }
  markLayoutDirty();
}

void Segmented::setOptionTooltip(std::size_t index, std::string_view text) {
  if (index < m_buttons.size() && m_buttons[index] != nullptr) {
    m_buttons[index]->setTooltip(text);
  }
}

void Segmented::clearOptions() {
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      (void)removeChild(btn);
    }
  }
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      (void)removeChild(sep);
    }
  }
  m_buttons.clear();
  m_separators.clear();
  m_selected = 0;
  markLayoutDirty();
}

void Segmented::setOnChange(std::function<void(std::size_t)> callback) { m_onChange = std::move(callback); }

void Segmented::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setEnabled(enabled);
    }
  }
  setOpacity(enabled ? 1.0f : 0.55f);
}

std::unique_ptr<Separator> Segmented::makeSegmentSeparator() {
  auto sep = std::make_unique<Separator>();
  sep->setOrientation(SeparatorOrientation::VerticalRule);
  sep->setThickness(std::max(1.0f, Style::borderWidth * m_scale));
  sep->setColor(colorSpecFromRole(ColorRole::Outline, 0.5f));
  sep->setFlexGrow(0.0f);
  return sep;
}

std::unique_ptr<Button>
Segmented::makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index) {
  auto btn = std::make_unique<Button>();
  if (!glyph.empty()) {
    btn->setGlyph(glyph);
    btn->setGlyphSize(effectiveFontSize());
  }
  if (!label.empty()) {
    btn->setText(label);
    btn->setFontSize(effectiveFontSize());
  }
  applyButtonMetrics(*btn);
  btn->setOnClick([this, index]() { setSelectedIndex(index); });
  btn->setFlexGrow(m_equalSegmentWidths ? 1.0f : 0.0f);
  btn->setContentAlign(ButtonContentAlign::Center);
  btn->setEnabled(m_enabled);
  return btn;
}

void Segmented::applyButtonMetrics(Button& button) const {
  if (m_compact) {
    button.setMinHeight((Style::fontSizeBody + Style::spaceXs * 2.0f) * m_scale);
    button.setPadding(Style::spaceXs * m_scale, Style::spaceXs * m_scale);
    return;
  }

  button.setMinHeight(Style::controlHeight * m_scale);
  button.setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
}

void Segmented::setEqualSegmentWidths(bool equalWidths) {
  if (m_equalSegmentWidths == equalWidths) {
    return;
  }
  m_equalSegmentWidths = equalWidths;
  for (Button* b : m_buttons) {
    if (b != nullptr) {
      b->setFlexGrow(m_equalSegmentWidths ? 1.0f : 0.0f);
    }
  }
  markLayoutDirty();
}

void Segmented::refreshVariants() {
  const std::size_t n = m_buttons.size();
  const float r = Style::scaledRadiusMd(m_scale);
  for (std::size_t i = 0; i < n; ++i) {
    if (m_buttons[i] == nullptr) {
      continue;
    }
    m_buttons[i]->setVariant(i == m_selected ? ButtonVariant::TabActive : ButtonVariant::Tab);
    Radii radii;
    if (n == 1) {
      radii = Radii{r, r, r, r};
    } else if (i == 0) {
      radii = Radii{r, 0.0f, 0.0f, r};
    } else if (i == n - 1) {
      radii = Radii{0.0f, r, r, 0.0f};
    } else {
      radii = Radii{0.0f};
    }
    m_buttons[i]->setRadii(radii);
  }
}

void Segmented::applyOuterStyle() {
  setPadding(0.0f);
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
  clearBorder();
  setRadius(Style::scaledRadiusMd(m_scale));
}

float Segmented::effectiveFontSize() const noexcept {
  return (m_fontSize > 0.0f ? m_fontSize : Style::fontSizeBody) * m_scale;
}
