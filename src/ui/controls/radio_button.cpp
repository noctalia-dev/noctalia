#include "ui/controls/radio_button.h"

#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

RadioButton::RadioButton() {
  auto outer = std::make_unique<Box>();
  m_outer = static_cast<Box*>(addChild(std::move(outer)));

  auto inner = std::make_unique<Box>();
  m_inner = static_cast<Box*>(addChild(std::move(inner)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled || m_checked) {
      return;
    }
    m_checked = true;
    applyState();
    if (m_onChange) {
      m_onChange(true);
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  applyState();
}

void RadioButton::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  applyState();
}

void RadioButton::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void RadioButton::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void RadioButton::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applyState();
  markLayoutDirty();
}

bool RadioButton::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool RadioButton::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void RadioButton::doLayout(Renderer& /*renderer*/) {
  const float touchSize = Style::controlHeightSm * m_scale;
  const float indicatorSize = (Style::fontSizeTitle + Style::spaceXs) * m_scale;
  const float indicatorInset = (touchSize - indicatorSize) * 0.5F;
  const float innerInset = (Style::spaceXs + Style::borderWidth) * m_scale;
  const float innerSize = indicatorSize - innerInset * 2.0F;

  setSize(touchSize, touchSize);

  if (m_outer != nullptr) {
    m_outer->setPosition(indicatorInset, indicatorInset);
    m_outer->setFrameSize(indicatorSize, indicatorSize);
    m_outer->setRadius(indicatorSize * 0.5F);
  }

  if (m_inner != nullptr) {
    m_inner->setPosition(indicatorInset + innerInset, indicatorInset + innerInset);
    m_inner->setFrameSize(innerSize, innerSize);
    m_inner->setRadius(innerSize * 0.5F);
  }

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setFrameSize(width(), height());
  }
}

void RadioButton::applyState() {
  if (m_outer == nullptr || m_inner == nullptr) {
    return;
  }

  ColorSpec fill = colorSpecFromRole(ColorRole::Surface);
  ColorSpec border = colorSpecFromRole(ColorRole::Outline);
  if (m_checked) {
    fill = colorSpecFromRole(ColorRole::Primary);
    border = colorSpecFromRole(ColorRole::Primary);
  } else if (hovered()) {
    border = colorSpecFromRole(ColorRole::Hover);
  }

  m_outer->setFill(fill);
  m_outer->setBorder(border, Style::borderWidth * m_scale);

  const ColorSpec innerFill =
      m_checked ? colorSpecFromRole(ColorRole::OnPrimary) : colorSpecFromRole(ColorRole::Surface);
  m_inner->setFill(innerFill);
  m_inner->setBorder(innerFill, 0.0F);
  m_inner->setVisible(m_checked);

  setOpacity(m_enabled ? 1.0F : 0.55F);
}
