#include "ui/controls/checkbox.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

Checkbox::Checkbox() {
  auto box = std::make_unique<Box>();
  m_box = static_cast<Box*>(addChild(std::move(box)));

  auto checkGlyph = std::make_unique<Glyph>();
  checkGlyph->setGlyph("check");
  m_checkGlyph = static_cast<Glyph*>(addChild(std::move(checkGlyph)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    const bool next = !m_checked;
    m_checked = next;
    applyState();
    if (m_onChange) {
      m_onChange(next);
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  applyState();
}

void Checkbox::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  applyState();
}

void Checkbox::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void Checkbox::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Checkbox::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  applyState();
  markLayoutDirty();
}

bool Checkbox::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Checkbox::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Checkbox::doLayout(Renderer& renderer) {
  const float touchSize = Style::controlHeightSm * m_scale;
  const float boxSize = (Style::fontSizeTitle + Style::spaceXs) * m_scale;
  const float boxInset = (touchSize - boxSize) * 0.5f;

  setSize(touchSize, touchSize);

  if (m_box != nullptr) {
    m_box->setPosition(boxInset, boxInset);
    m_box->setFrameSize(boxSize, boxSize);
    m_box->setRadius(Style::scaledRadiusSm(m_scale));
  }

  if (m_checkGlyph != nullptr) {
    m_checkGlyph->setGlyphSize((Style::fontSizeBody + Style::spaceXs * 0.5f) * m_scale);
    m_checkGlyph->measure(renderer);
    m_checkGlyph->setPosition(
        std::round(boxInset + (boxSize - m_checkGlyph->width()) * 0.5f),
        std::round(boxInset + (boxSize - m_checkGlyph->height()) * 0.5f)
    );
  }

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

void Checkbox::applyState() {
  if (m_box == nullptr || m_checkGlyph == nullptr) {
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

  m_box->setFill(fill);
  m_box->setBorder(border, Style::borderWidth * m_scale);

  m_checkGlyph->setColor(colorSpecFromRole(ColorRole::OnPrimary));
  m_checkGlyph->setVisible(m_checked);

  setOpacity(m_enabled ? 1.0f : 0.55f);
}
