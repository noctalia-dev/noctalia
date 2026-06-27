#include "ui/controls/toggle.h"

#include "core/keybind_matcher.h"
#include "render/animation/animation_manager.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

Toggle::Toggle() {
  setAlign(FlexAlign::Center);
  setDirection(FlexDirection::Horizontal);
  setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);

  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    activateFromInput();
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() { applyState(); });
  area->setOnFocusLoss([this]() { applyState(); });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      activateFromInput();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);

  applySize();
  applyState();
  m_paletteConn = paletteChanged().connect([this] { applyAnimatedState(m_animationProgress); });
}

void Toggle::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;

  if (animationManager() != nullptr) {
    if (m_animId != 0) {
      animationManager()->cancel(m_animId);
    }
    float from = m_checked ? 0.0f : 1.0f;
    float to = m_checked ? 1.0f : 0.0f;
    m_animId = animationManager()->animate(
        from, to, Style::animFast, Easing::EaseOutCubic, [this](float t) { applyAnimatedState(t); },
        [this]() { m_animId = 0; }, this
    );
    // Mark dirty so the surface's frame loop restarts and ticks the animation
    markPaintDirty();
  } else {
    applyState();
  }
}

void Toggle::setCheckedImmediate(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
  applyState();
}

void Toggle::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void Toggle::setToggleSize(ToggleSize size) {
  if (m_size == size) {
    return;
  }
  m_size = size;
  applySize();
  applyState();
}

void Toggle::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  applySize();
  applyState();
  markLayoutDirty();
}

void Toggle::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Toggle::setTabFocusKey(std::string key) {
  if (m_inputArea != nullptr) {
    m_inputArea->setTabFocusKey(std::move(key));
  }
}

void Toggle::activateFromInput() {
  const bool next = !m_checked;
  setChecked(next);
  if (m_onChange) {
    m_onChange(next);
  }
}

bool Toggle::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Toggle::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Toggle::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize Toggle::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Toggle::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Toggle::applySize() {
  switch (m_size) {
  case ToggleSize::Small:
    m_thumbSize = Style::toggleThumbSizeSm * m_scale;
    m_inset = Style::toggleInsetSm * m_scale;
    m_travel = Style::toggleTravelSm * m_scale;
    break;
  case ToggleSize::Medium:
    m_thumbSize = Style::toggleThumbSizeMd * m_scale;
    m_inset = Style::toggleInsetMd * m_scale;
    m_travel = Style::toggleTravelMd * m_scale;
    break;
  case ToggleSize::Large:
    m_thumbSize = Style::toggleThumbSizeLg * m_scale;
    m_inset = Style::toggleInsetLg * m_scale;
    m_travel = Style::toggleTravelLg * m_scale;
    break;
  }

  m_thumb->setFrameSize(m_thumbSize, m_thumbSize);
  setRadius((m_thumbSize + (m_inset * 2.0f)) * 0.5f);
}

void Toggle::applyState() { applyAnimatedState(m_checked ? 1.0f : 0.0f); }

void Toggle::applyAnimatedState(float t) {
  m_animationProgress = t;
  const Color trackColor = lerpColor(colorForRole(ColorRole::Outline), colorForRole(ColorRole::Primary), t);
  const Color thumbColor = lerpColor(colorForRole(ColorRole::OnPrimary), colorForRole(ColorRole::OnPrimary), t);
  const float thumbX = m_inset + m_travel * t;
  ColorSpec borderColor = colorSpecFromRole(ColorRole::Outline);

  if (m_enabled) {
    if (m_inputArea != nullptr && m_inputArea->focused()) {
      borderColor = focusRingColorSpec();
    } else if (hovered()) {
      borderColor = colorSpecFromRole(ColorRole::Hover);
    } else if (m_checked) {
      borderColor = colorSpecFromRole(ColorRole::Primary);
    }
  }

  setFill(trackColor);
  setBorder(borderColor, m_inputArea != nullptr && m_inputArea->focused() ? Style::focusRingWidth : Style::borderWidth);
  m_thumb->setPosition(thumbX, m_inset);

  auto thumbStyle = m_thumb->style();
  thumbStyle.fillMode = FillMode::Solid;
  thumbStyle.radius = m_thumbSize * 0.5f;
  thumbStyle.softness = 1.0f;
  thumbStyle.borderWidth = 0.0f;
  thumbStyle.fill = thumbColor;
  m_thumb->setStyle(thumbStyle);

  // Padding keeps Flex size consistent regardless of thumb position
  const float rightPad = m_inset + m_travel - (thumbX - m_inset);
  setPadding(m_inset, rightPad, m_inset, thumbX);

  if (m_enabled) {
    setOpacity(1.0f);
  } else {
    setOpacity(0.55f);
  }
}
