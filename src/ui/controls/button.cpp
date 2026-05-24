#include "ui/controls/button.h"

#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>
#include <memory>

namespace {

  Button::ButtonStateColors makeState(ColorSpec bg, ColorSpec border, ColorSpec label) {
    return Button::ButtonStateColors{
        .bg = std::move(bg),
        .border = std::move(border),
        .label = std::move(label),
    };
  }

  Button::ButtonPalette paletteForVariant(ButtonVariant variant) {
    constexpr float kDisabledAlpha = 0.55f;
    switch (variant) {
    case ButtonVariant::Default:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal = makeState(
              colorSpecFromRole(ColorRole::SurfaceVariant), colorSpecFromRole(ColorRole::Outline),
              colorSpecFromRole(ColorRole::OnSurface)
          ),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Primary),
              colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .disabled = makeState(
              colorSpecFromRole(ColorRole::SurfaceVariant, kDisabledAlpha),
              colorSpecFromRole(ColorRole::Outline, kDisabledAlpha),
              colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha)
          ),
      };
    case ButtonVariant::Primary:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal = makeState(
              colorSpecFromRole(ColorRole::Primary), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::Primary), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .disabled = makeState(
              colorSpecFromRole(ColorRole::Primary, kDisabledAlpha), clearColorSpec(),
              colorSpecFromRole(ColorRole::OnPrimary)
          ),
      };
    case ButtonVariant::Secondary:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal = makeState(
              colorSpecFromRole(ColorRole::Secondary), colorSpecFromRole(ColorRole::Outline),
              colorSpecFromRole(ColorRole::OnSecondary)
          ),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Primary),
              colorSpecFromRole(ColorRole::OnPrimary)
          ),

          .disabled = makeState(
              colorSpecFromRole(ColorRole::Secondary, kDisabledAlpha),
              colorSpecFromRole(ColorRole::Outline, kDisabledAlpha), colorSpecFromRole(ColorRole::OnSecondary)
          ),
      };
    case ButtonVariant::Destructive:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal = makeState(
              colorSpecFromRole(ColorRole::Error), colorSpecFromRole(ColorRole::Outline),
              colorSpecFromRole(ColorRole::OnError)
          ),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::Error), colorSpecFromRole(ColorRole::Error),
              colorSpecFromRole(ColorRole::OnError)
          ),
          .disabled = makeState(
              colorSpecFromRole(ColorRole::Error, kDisabledAlpha),
              colorSpecFromRole(ColorRole::Outline, kDisabledAlpha), colorSpecFromRole(ColorRole::OnError)
          ),
      };
    case ButtonVariant::Outline:
      return Button::ButtonPalette{
          .borderWidth = Style::borderWidth,
          .normal = makeState(
              colorSpecFromRole(ColorRole::Surface), colorSpecFromRole(ColorRole::Outline),
              colorSpecFromRole(ColorRole::OnSurface)
          ),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),

          .pressed = makeState(
              colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Primary),
              colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .disabled = makeState(
              colorSpecFromRole(ColorRole::Surface, kDisabledAlpha),
              colorSpecFromRole(ColorRole::Outline, kDisabledAlpha),
              colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha)
          ),
      };
    case ButtonVariant::Ghost:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal = makeState(clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::SurfaceVariant), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)
          ),
          .disabled =
              makeState(clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface, kDisabledAlpha)),
      };
    case ButtonVariant::Tab:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal = makeState(clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)),
          .hover =
              makeState(colorSpecFromRole(ColorRole::Hover), clearColorSpec(), colorSpecFromRole(ColorRole::OnHover)),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::SurfaceVariant), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)
          ),
          .disabled = makeState(clearColorSpec(), clearColorSpec(), colorSpecFromRole(ColorRole::OnSurface)),
      };
    case ButtonVariant::TabActive:
      return Button::ButtonPalette{
          .borderWidth = 0.0f,
          .normal = makeState(
              colorSpecFromRole(ColorRole::Primary), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .hover = makeState(
              colorSpecFromRole(ColorRole::Primary), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .pressed = makeState(
              colorSpecFromRole(ColorRole::Primary), clearColorSpec(), colorSpecFromRole(ColorRole::OnPrimary)
          ),
          .disabled = makeState(
              colorSpecFromRole(ColorRole::Primary, kDisabledAlpha), clearColorSpec(),
              colorSpecFromRole(ColorRole::OnPrimary)
          ),
      };
    }

    return {};
  }

} // namespace

Button::Button() {
  setAlign(FlexAlign::Center);
  setMinHeight(Style::controlHeightSm);
  setPadding(Style::spaceSm);
  setRadius(Style::scaledRadiusMd());

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    if (m_onEnter) {
      m_onEnter();
    }
  });
  area->setOnLeave([this]() {
    applyVisualState();
    if (m_onLeave) {
      m_onLeave();
    }
  });
  area->setOnPress([this](const InputArea::PointerData& data) {
    applyVisualState();
    if (m_onPress) {
      m_onPress(data.localX, data.localY, data.pressed);
    }
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_onMotion) {
      m_onMotion();
    }
    if (m_onPointerMotion) {
      m_onPointerMotion(data.localX, data.localY);
    }
  });
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (!m_enabled) {
      return;
    }
    if (data.button == BTN_RIGHT && m_onRightClick) {
      m_onRightClick();
    } else if (data.button == BTN_LEFT && m_onClick) {
      m_onClick();
    }
  });
  area->setEnabled(false);
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);
  m_inputArea->setPosition(0.0f, 0.0f);
  m_inputArea->setFrameSize(width(), height());

  applyVariant();
  m_paletteConn = paletteChanged().connect([this] {
    // Re-derive color slots from the (possibly updated) palette and push
    // them immediately if no hover/press animation is in flight. Otherwise
    // the running animation keeps its old snapshot for one more tick and
    // the next applyVisualState() will resync.
    applyVariant();
  });
}

Button::~Button() {
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
}

void Button::setText(std::string_view text) {
  ensureLabel();
  m_label->setText(text);
  m_label->setVisible(!text.empty());
}

void Button::setGlyph(std::string_view name) {
  ensureGlyph();
  m_glyph->setGlyph(name);
}

void Button::setFontSize(float size) {
  ensureLabel();
  m_label->setFontSize(size);
  if (m_glyph != nullptr) {
    m_glyph->setGlyphSize(size);
  }
}

void Button::setGlyphSize(float size) {
  ensureGlyph();
  m_glyph->setGlyphSize(size);
}

void Button::setOnClick(std::function<void()> callback) {
  m_onClick = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setOnRightClick(std::function<void()> callback) {
  m_onRightClick = std::move(callback);
  if (m_inputArea != nullptr) {
    m_inputArea->setAcceptedButtons(
        m_onRightClick ? InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}) : InputArea::buttonMask(BTN_LEFT)
    );
  }
  refreshInputAreaEnabled();
}

void Button::setOnPress(std::function<void(float, float, bool)> callback) {
  m_onPress = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setOnMotion(std::function<void()> callback) {
  m_onMotion = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setOnPointerMotion(std::function<void(float, float)> callback) {
  m_onPointerMotion = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setOnEnter(std::function<void()> callback) {
  m_onEnter = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setOnLeave(std::function<void()> callback) {
  m_onLeave = std::move(callback);
  refreshInputAreaEnabled();
}

void Button::setHoverSuppressed(bool suppressed) {
  if (m_hoverSuppressed == suppressed) {
    return;
  }
  m_hoverSuppressed = suppressed;
  applyVisualState();
}

void Button::setCursorShape(std::uint32_t shape) {
  if (m_inputArea != nullptr) {
    m_inputArea->setCursorShape(shape);
  }
}

void Button::setBadge(std::string_view text) {
  ensureBadge();
  m_badgeLabel->setText(text);
  m_badge->setVisible(!text.empty());
}

void Button::setBadgeFontSize(float size) {
  ensureBadge();
  m_badgeLabel->setFontSize(size);
}

void Button::setTooltip(std::string_view text) {
  if (m_inputArea == nullptr) {
    return;
  }
  if (text.empty()) {
    m_inputArea->clearTooltip();
  } else {
    m_inputArea->setTooltip(std::string(text));
  }
}

void Button::ensureBadge() {
  if (m_badge != nullptr) {
    return;
  }
  auto badge = std::make_unique<Flex>();
  badge->setDirection(FlexDirection::Horizontal);
  badge->setAlign(FlexAlign::Center);
  badge->setJustify(FlexJustify::Center);
  badge->setPadding(2.0f, Style::spaceXs);
  badge->setRadius(Style::scaledRadiusSm());
  badge->setParticipatesInLayout(false);
  badge->setVisible(false);

  auto label = std::make_unique<Label>();
  label->setFontSize(Style::fontSizeCaption * 0.85f);
  m_badgeLabel = static_cast<Label*>(badge->addChild(std::move(label)));

  m_badge = static_cast<Flex*>(addChild(std::move(badge)));
}

void Button::updateInputArea() {
  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setFrameSize(width(), height());
  }
}

bool Button::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Button::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Button::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  refreshInputAreaEnabled();
  applyVisualState();
}

void Button::setSelected(bool selected) {
  if (m_selected == selected) {
    return;
  }
  m_selected = selected;
  applyVisualState();
}

void Button::setContentAlign(ButtonContentAlign align) { m_contentAlign = align; }

void Button::setVariant(ButtonVariant variant) {
  if (m_variant == variant) {
    return;
  }
  m_variant = variant;
  m_customPalette.reset();
  applyVariant();
}

void Button::setCustomPalette(ButtonPalette customPalette) {
  m_customPalette = std::move(customPalette);
  applyVariant();
}

void Button::applyVariant() {
  m_palette = m_customPalette.value_or(paletteForVariant(m_variant));
  setBorder(m_palette.normal.border, m_palette.borderWidth);

  // Only seed targets before the first visual state application. Once the
  // button has been painted, applyVisualState() must compare against the
  // previous targets so variant/palette changes actually propagate.
  if (!m_visualStateInitialized) {
    m_targetBg = resolveColorSpec(m_palette.normal.bg);
    m_targetBorder = resolveColorSpec(m_palette.normal.border);
    m_targetLabel = resolveColorSpec(m_palette.normal.label);
  }
  applyVisualState();
}

void Button::refreshInputAreaEnabled() {
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(
        m_enabled && (static_cast<bool>(m_onClick) || static_cast<bool>(m_onMotion) ||
                      static_cast<bool>(m_onPointerMotion) || static_cast<bool>(m_onPress) ||
                      static_cast<bool>(m_onEnter) || static_cast<bool>(m_onLeave) || static_cast<bool>(m_onRightClick))
    );
  }
}

void Button::ensureLabel() {
  if (m_label != nullptr) {
    return;
  }
  auto label = std::make_unique<Label>();
  m_label = static_cast<Label*>(addChild(std::move(label)));
  setMinHeight(Style::controlHeight);
  setPadding(Style::spaceSm, Style::spaceMd);
  if (m_glyph != nullptr) {
    setDirection(FlexDirection::Horizontal);
    setGap(Style::spaceXs);
  }
  applyColors(m_targetBg, m_targetBorder, m_targetLabel);
}

void Button::ensureGlyph() {
  if (m_glyph != nullptr) {
    return;
  }
  // insertChildAt so the glyph lands before the label in the children vector,
  // which is what Flex iterates to assign layout positions
  if (m_label != nullptr) {
    auto& kids = children();
    std::size_t labelIndex = 0;
    for (std::size_t i = 0; i < kids.size(); ++i) {
      if (kids[i].get() == m_label) {
        labelIndex = i;
        break;
      }
    }
    auto glyph = std::make_unique<Glyph>();
    m_glyph = static_cast<Glyph*>(insertChildAt(labelIndex, std::move(glyph)));
  } else {
    auto glyph = std::make_unique<Glyph>();
    m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));
  }
  if (m_label != nullptr) {
    setDirection(FlexDirection::Horizontal);
    setGap(Style::spaceXs);
  }
  applyColors(m_targetBg, m_targetBorder, m_targetLabel);
}

void Button::applyColors(const Color& bg, const Color& border, const Color& label) {
  setFill(bg);
  setBorder(border, m_palette.borderWidth);
  if (m_label != nullptr) {
    m_label->setColor(label);
  }
  if (m_glyph != nullptr) {
    m_glyph->setColor(label);
  }
  for (auto& child : children()) {
    if (child.get() == m_label || child.get() == m_glyph || child.get() == m_badge) {
      continue;
    }
    if (auto* lbl = dynamic_cast<Label*>(child.get())) {
      lbl->setColor(label);
    } else if (auto* gl = dynamic_cast<Glyph*>(child.get())) {
      gl->setColor(label);
    }
  }
  if (m_badge != nullptr) {
    m_badge->setFill(Color{label.r, label.g, label.b, label.a * 0.85f});
    if (m_badgeLabel != nullptr) {
      m_badgeLabel->setColor(bg);
    }
  }
  m_visualStateInitialized = true;
}

void Button::resolveVisualStateColors(Color& targetBg, Color& targetBorder, Color& targetLabel) const {
  bool isHovered = m_enabled && ((!m_hoverSuppressed && hovered()) || m_selected);
  bool isPressed = m_enabled && pressed();

  if (!m_enabled) {
    targetBg = resolveColorSpec(m_palette.disabled.bg);
    targetBorder = resolveColorSpec(m_palette.disabled.border);
    targetLabel = resolveColorSpec(m_palette.disabled.label);
  } else if (isPressed) {
    targetBg = resolveColorSpec(m_palette.pressed.bg);
    targetBorder = resolveColorSpec(m_palette.pressed.border);
    targetLabel = resolveColorSpec(m_palette.pressed.label);
  } else if (isHovered) {
    targetBg = resolveColorSpec(m_palette.hover.bg);
    targetBorder = resolveColorSpec(m_palette.hover.border);
    targetLabel = resolveColorSpec(m_palette.hover.label);
  } else {
    targetBg = resolveColorSpec(m_palette.normal.bg);
    targetBorder = resolveColorSpec(m_palette.normal.border);
    targetLabel = resolveColorSpec(m_palette.normal.label);
  }
}

void Button::applyVisualState() {
  Color targetBg;
  Color targetBorder;
  Color targetLabel;
  resolveVisualStateColors(targetBg, targetBorder, targetLabel);

  if (!m_visualStateInitialized) {
    m_targetBg = targetBg;
    m_targetBorder = targetBorder;
    m_targetLabel = targetLabel;
    applyColors(targetBg, targetBorder, targetLabel);
    return;
  }

  if (targetBg == m_targetBg && targetBorder == m_targetBorder && targetLabel == m_targetLabel) {
    return;
  }

  if (animationManager() == nullptr) {
    applyColors(targetBg, targetBorder, targetLabel);
    m_targetBg = targetBg;
    m_targetBorder = targetBorder;
    m_targetLabel = targetLabel;
    return;
  }

  // Snapshot current display colors as the animation start point
  m_fromBg = m_targetBg;
  m_fromBorder = m_targetBorder;
  m_fromLabel = m_targetLabel;
  m_targetBg = targetBg;
  m_targetBorder = targetBorder;
  m_targetLabel = targetLabel;

  if (m_animId != 0) {
    animationManager()->cancel(m_animId);
  }

  m_animId = animationManager()->animate(
      0.0f, 1.0f, Style::animFast, Easing::EaseOutCubic,
      [this](float t) {
        applyColors(
            lerpColor(m_fromBg, m_targetBg, t), lerpColor(m_fromBorder, m_targetBorder, t),
            lerpColor(m_fromLabel, m_targetLabel, t)
        );
      },
      [this]() { m_animId = 0; }
  );
  markPaintDirty();
}

void Button::doLayout(Renderer& renderer) {
  const bool useCurrentSize = arrangingByLayout() || !sizeAssignedByLayout();
  const float assignedWidth = useCurrentSize ? width() : 0.0f;
  const float assignedHeight = useCurrentSize ? height() : 0.0f;
  const bool hasVisibleLabel = m_label != nullptr && m_label->visible();
  const bool glyphOnly = m_glyph != nullptr && !hasVisibleLabel;

  if (m_label != nullptr) {
    m_label->measure(renderer);
  }
  if (m_glyph != nullptr) {
    m_glyph->measure(renderer);
  }

  Flex::doLayout(renderer);

  // Buttons are often sized by a parent stretch pass. Preserve that assigned
  // box instead of collapsing back to intrinsic content width.
  if (assignedWidth > 0.0f || assignedHeight > 0.0f) {
    setSize(std::max(width(), assignedWidth), std::max(height(), assignedHeight));
  }

  if (glyphOnly && m_contentAlign == ButtonContentAlign::Center) {
    const bool hasAssignedWidth = assignedWidth > 0.0f;
    if (!hasAssignedWidth) {
      const float squareSize = std::max(width(), height());
      setSize(squareSize, squareSize);
    }
  }

  // After Flex layout the content row is left-anchored inside the padding.
  // Shift the whole group to honour m_contentAlign (Start leaves it as-is).
  if (m_contentAlign != ButtonContentAlign::Start) {
    float contentLeft = 0.0f;
    float contentRight = 0.0f;
    float contentTop = 0.0f;
    float contentBottom = 0.0f;
    bool haveContent = false;

    for (auto& child : children()) {
      Node* node = child.get();
      if (node == nullptr || !node->visible() || !node->participatesInLayout() || node->zIndex() < 0) {
        continue;
      }
      const float left = node->x();
      const float right = node->x() + node->width();
      const float top = node->y();
      const float bottom = node->y() + node->height();
      if (!haveContent) {
        contentLeft = left;
        contentRight = right;
        contentTop = top;
        contentBottom = bottom;
        haveContent = true;
      } else {
        contentLeft = std::min(contentLeft, left);
        contentRight = std::max(contentRight, right);
        contentTop = std::min(contentTop, top);
        contentBottom = std::max(contentBottom, bottom);
      }
    }

    if (haveContent) {
      const float contentWidth = contentRight - contentLeft;
      const float contentHeight = contentBottom - contentTop;
      float targetLeft = 0.0f;
      if (m_contentAlign == ButtonContentAlign::Center) {
        targetLeft = std::round((width() - contentWidth) * 0.5f);
      } else { // End
        targetLeft = std::round(width() - contentWidth - paddingRight());
      }
      const float shiftX = targetLeft - contentLeft;
      const float targetTop = std::round((height() - contentHeight) * 0.5f);
      const float shiftY = targetTop - contentTop;
      if (std::abs(shiftX) > 0.01f || std::abs(shiftY) > 0.01f) {
        for (auto& child : children()) {
          Node* node = child.get();
          if (node == nullptr || !node->visible() || !node->participatesInLayout() || node->zIndex() < 0) {
            continue;
          }
          node->setPosition(node->x() + shiftX, node->y() + shiftY);
        }
      }
    }
  }

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0f, 0.0f);
    m_inputArea->setSize(width(), height());
  }

  if (m_badge != nullptr && m_badge->visible()) {
    m_badgeLabel->measure(renderer);
    m_badge->setMinWidth(0.0f);
    m_badge->layout(renderer);
    m_badge->setMinWidth(m_badge->height());
    m_badge->layout(renderer);
    const float margin = Style::spaceXs;
    m_badge->setPosition(width() - m_badge->width() - margin, margin);
  }

  // Only apply visual state if no animation is in progress — a running
  // animation already owns the color transition and re-calling here would
  // reset its from/to snapshot, collapsing the animation to a no-op.
  if (m_animId == 0) {
    applyVisualState();
  }
}

LayoutSize Button::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Button::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }
