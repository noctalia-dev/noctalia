#include "ui/controls/select.h"

#include "core/input/keybind_matcher.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/box.h"
#include "ui/controls/color_swatch_preview.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/select_popup_context.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>

namespace {

  constexpr float kMaxVisibleOptions = 6;
  constexpr float kPlaceholderAlpha = 0.68f;

  Color resolved(ColorRole role, float alpha = 1.0f) { return colorForRole(role, alpha); }

} // namespace

Select::Select() {
  setMinWidth(80.0f);
  m_placeholder = i18n::tr("ui.controls.select.placeholder");

  auto triggerBackground = std::make_unique<RectNode>();
  m_triggerBackground = static_cast<RectNode*>(addChild(std::move(triggerBackground)));

  auto triggerIndicator = std::make_unique<Box>();
  triggerIndicator->setVisible(false);
  m_triggerIndicator = static_cast<Box*>(addChild(std::move(triggerIndicator)));

  auto triggerPreview = std::make_unique<ColorSwatchPreviewStrip>();
  triggerPreview->setVisible(false);
  triggerPreview->setParticipatesInLayout(false);
  m_triggerPreview = static_cast<ColorSwatchPreviewStrip*>(addChild(std::move(triggerPreview)));

  auto triggerLabel = std::make_unique<Label>();
  triggerLabel->setMaxLines(1);
  m_triggerLabel = static_cast<Label*>(addChild(std::move(triggerLabel)));

  auto triggerGlyph = std::make_unique<Glyph>();
  m_triggerGlyph = static_cast<Glyph*>(addChild(std::move(triggerGlyph)));
  m_triggerGlyph->setGlyph("chevron-down");
  m_triggerGlyph->setGlyphSize(m_glyphSize);

  auto triggerArea = std::make_unique<InputArea>();
  triggerArea->setFocusable(true);
  triggerArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  triggerArea->setOnEnter([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    markPaintDirty();
  });
  triggerArea->setOnLeave([this]() {
    applyVisualState();
    markPaintDirty();
  });
  triggerArea->setOnPress([this](const InputArea::PointerData& /*data*/) {
    applyVisualState();
    markPaintDirty();
  });
  triggerArea->setOnClick([this](const InputArea::PointerData& data) {
    if (!m_enabled || data.button != BTN_LEFT) {
      return;
    }
    toggleOpen();
  });
  triggerArea->setOnFocusGain([this]() {
    applyVisualState();
    markPaintDirty();
  });
  triggerArea->setOnFocusLoss([this]() {
    applyVisualState();
    markPaintDirty();
  });
  triggerArea->setOnKeyDown([this](const InputArea::KeyData& key) {
    handleKey(key.sym, key.utf32, key.modifiers, key.pressed);
  });
  m_triggerArea = static_cast<InputArea*>(addChild(std::move(triggerArea)));

  applyVisualState();
  m_paletteConn = paletteChanged().connect([this] { applyVisualState(); });
  m_inputBordersConn = Style::inputBordersChanged().connect([this] { applyVisualState(); });
}

Select::~Select() {
  if (m_caretAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_caretAnimId);
  }
  if (m_open) {
    auto* ctx = popupContext();
    if (ctx != nullptr && ctx->isSelectDropdownOpen()) {
      ctx->closeSelectDropdown();
    }
  }
}

void Select::setOptions(std::vector<std::string> options) {
  m_options = std::move(options);
  if (m_options.empty()) {
    m_selectedIndex = npos;
  } else if (m_selectedIndex == npos || m_selectedIndex >= m_options.size()) {
    m_selectedIndex = 0;
  }
  syncTriggerText();
  applyVisualState();
  markLayoutDirty();
}

void Select::setSelectedIndex(std::size_t index) { setSelectedIndexInternal(index, true); }

void Select::setSelectedIndexSilently(std::size_t index) { setSelectedIndexInternal(index, false); }

void Select::setSelectedIndexInternal(std::size_t index, bool notify) {
  if (index >= m_options.size()) {
    return;
  }
  if (m_selectedIndex == index) {
    if (notify && m_notifyOnReselect && m_onSelectionChanged) {
      m_onSelectionChanged(m_selectedIndex, selectedText());
    }
    return;
  }
  m_selectedIndex = index;
  syncTriggerText();
  applyVisualState();
  markLayoutDirty();
  if (notify && m_onSelectionChanged) {
    m_onSelectionChanged(m_selectedIndex, selectedText());
  }
}

void Select::clearSelection() {
  if (m_selectedIndex == npos) {
    return;
  }
  m_selectedIndex = npos;
  syncTriggerText();
  applyVisualState();
  markLayoutDirty();
}

void Select::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  applyVisualState();
}

void Select::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (!m_enabled && m_open) {
    closeMenu();
  }
  applyVisualState();
  markPaintDirty();
}

void Select::setPlaceholder(std::string_view placeholder) {
  m_placeholder = std::string(placeholder);
  syncTriggerText();
  applyVisualState();
  markLayoutDirty();
}

void Select::setFontSize(float size) {
  m_fontSize = std::max(1.0f, size);
  syncTriggerText();
  markLayoutDirty();
}

void Select::setControlHeight(float height) {
  m_controlHeight = std::max(1.0f, height);
  markLayoutDirty();
}

void Select::setHorizontalPadding(float padding) {
  m_horizontalPadding = std::max(0.0f, padding);
  markLayoutDirty();
}

void Select::setGlyphSize(float size) {
  m_glyphSize = std::max(1.0f, size);
  if (m_triggerGlyph != nullptr) {
    m_triggerGlyph->setGlyphSize(m_glyphSize);
  }
  markLayoutDirty();
}

void Select::setOptionIndicators(std::vector<ColorSpec> colors) {
  m_indicatorColors = std::move(colors);
  if (m_triggerIndicator != nullptr && m_selectedIndex < m_indicatorColors.size()) {
    m_triggerIndicator->setFill(m_indicatorColors[m_selectedIndex]);
  }
  applyVisualState();
  markPaintDirty();
  markLayoutDirty();
}

void Select::setColorSwatchPreviews(std::vector<ColorSwatchPreview> previews) {
  m_optionSwatchPreviews = std::move(previews);
  markLayoutDirty();
}

void Select::setNotifyOnReselect(bool enabled) { m_notifyOnReselect = enabled; }

void Select::setOnSelectionChanged(std::function<void(std::size_t, std::string_view)> callback) {
  m_onSelectionChanged = std::move(callback);
}

std::string_view Select::selectedText() const noexcept {
  if (m_selectedIndex >= m_options.size()) {
    return {};
  }
  return m_options[m_selectedIndex];
}

void Select::doLayout(Renderer& renderer) {
  if (m_triggerBackground == nullptr
      || m_triggerLabel == nullptr
      || m_triggerGlyph == nullptr
      || m_triggerArea == nullptr) {
    return;
  }

  m_fixedWidth = width() > 0.0f ? width() : 0.0f;

  syncTriggerText();
  m_triggerLabel->measure(renderer);
  m_triggerGlyph->measure(renderer);

  const bool hasSelectedPreview =
      m_selectedIndex < m_optionSwatchPreviews.size() && !m_optionSwatchPreviews[m_selectedIndex].empty();
  if (m_triggerPreview != nullptr) {
    m_triggerPreview->setMetricsFromFontSize(m_fontSize);
    if (hasSelectedPreview) {
      m_triggerPreview->setPreview(m_optionSwatchPreviews[m_selectedIndex]);
    }
    m_triggerPreview->setVisible(hasSelectedPreview);
    m_triggerPreview->setParticipatesInLayout(hasSelectedPreview);
  }

  const float previewWidth =
      hasSelectedPreview && m_triggerPreview != nullptr ? m_triggerPreview->preferredWidth() : 0.0f;
  const float previewHeight =
      hasSelectedPreview && m_triggerPreview != nullptr ? m_triggerPreview->preferredHeight() : 0.0f;
  const bool hasIndicators = !hasSelectedPreview && !m_indicatorColors.empty();
  const float indicatorSize = hasIndicators ? std::round(m_fontSize) : 0.0f;
  const float indicatorBorder = hasIndicators ? 1.5f : 0.0f;
  const float leadingInset =
      hasSelectedPreview ? (previewWidth + Style::spaceSm) : (hasIndicators ? indicatorSize + Style::spaceSm : 0.0f);

  float contentWidth =
      m_triggerLabel->width() + m_horizontalPadding * 2.0f + m_glyphSize + Style::spaceXs + leadingInset;
  float dropdownWidth = m_fixedWidth > 0.0f ? m_fixedWidth : std::max(minWidth(), contentWidth);

  setSize(dropdownWidth, m_controlHeight);

  m_triggerBackground->setPosition(0.0f, 0.0f);
  m_triggerBackground->setFrameSize(dropdownWidth, m_controlHeight);

  if (m_triggerIndicator != nullptr) {
    const bool showTriggerIndicator = hasIndicators && m_selectedIndex < m_indicatorColors.size();
    m_triggerIndicator->setVisible(showTriggerIndicator);
    if (showTriggerIndicator) {
      m_triggerIndicator->setFill(m_indicatorColors[m_selectedIndex]);
      m_triggerIndicator->setBorder(colorSpecFromRole(ColorRole::Outline), indicatorBorder);
      m_triggerIndicator->setFrameSize(indicatorSize, indicatorSize);
      m_triggerIndicator->setRadius(indicatorSize * 0.5f);
      m_triggerIndicator->setPosition(m_horizontalPadding, std::round((m_controlHeight - indicatorSize) * 0.5f));
    }
  }

  if (m_triggerPreview != nullptr && hasSelectedPreview) {
    m_triggerPreview->setPosition(m_horizontalPadding, std::round((m_controlHeight - previewHeight) * 0.5f));
  }

  const float triggerLabelLeft = m_horizontalPadding + leadingInset;
  const float triggerLabelMax =
      std::max(0.0f, dropdownWidth - (triggerLabelLeft + m_horizontalPadding + m_glyphSize + Style::spaceXs));
  m_triggerLabel->setMaxWidth(triggerLabelMax);
  m_triggerLabel->measure(renderer);
  float triggerLabelY = std::round((m_controlHeight - m_triggerLabel->height()) * 0.5f);
  float triggerGlyphY = std::round((m_controlHeight - m_triggerGlyph->height()) * 0.5f);
  m_triggerLabel->setPosition(triggerLabelLeft, triggerLabelY);
  m_triggerGlyph->setPosition(dropdownWidth - m_horizontalPadding - m_triggerGlyph->width(), triggerGlyphY);
  m_triggerArea->setPosition(0.0f, 0.0f);
  m_triggerArea->setFrameSize(dropdownWidth, m_controlHeight);

  applyVisualState();
}

LayoutSize Select::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Select::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Select::handleKey(std::uint32_t sym, std::uint32_t /*utf32*/, std::uint32_t modifiers, bool pressed) {
  if (!m_enabled || !pressed) {
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (!m_open) {
      toggleOpen();
    }
  }
}

void Select::applyVisualState() {
  if (m_triggerLabel == nullptr || m_triggerGlyph == nullptr || m_triggerBackground == nullptr) {
    return;
  }

  const bool triggerHovered = m_triggerArea != nullptr && m_triggerArea->hovered();
  const bool triggerPressed = m_triggerArea != nullptr && m_triggerArea->pressed();
  const bool triggerFocused = m_triggerArea != nullptr && m_triggerArea->focused();

  Color triggerBg = resolved(ColorRole::SurfaceVariant, m_surfaceOpacity);
  Color triggerBorder = resolved(ColorRole::Outline);
  ColorSpec triggerText = selectedText().empty() ? colorSpecFromRole(ColorRole::OnSurfaceVariant, kPlaceholderAlpha)
                                                 : colorSpecFromRole(ColorRole::OnSurface);

  if (!m_enabled) {
    triggerBg = resolved(ColorRole::SurfaceVariant, m_surfaceOpacity * 0.75f);
    triggerBorder = resolved(ColorRole::Outline, Style::disabledOutlineAlpha);
    triggerText = colorSpecFromRole(ColorRole::OnSurface, 0.55f);
  } else if (triggerHovered || triggerPressed) {
    triggerBg = resolved(ColorRole::SurfaceVariant, m_surfaceOpacity);
    triggerBorder = resolved(ColorRole::Hover);
  } else if (triggerFocused) {
    triggerBorder = resolveColorSpec(focusRingColorSpec());
  }

  m_triggerLabel->setColor(triggerText);
  m_triggerGlyph->setColor(triggerText);
  m_triggerGlyph->setRotation(m_caretProgress * std::numbers::pi_v<float>);

  float resolvedBorderWidth = 0.0f;
  if (triggerFocused) {
    resolvedBorderWidth = Style::focusRingWidth;
  } else if (Style::inputBordersEnabled()) {
    resolvedBorderWidth = Style::borderWidth;
  }

  m_triggerBackground->setStyle(
      RoundedRectStyle{
          .fill = triggerBg,
          .border = triggerBorder,
          .fillMode = FillMode::Solid,
          .radius = Style::scaledRadiusMd(),
          .softness = 1.0f,
          .borderWidth = resolvedBorderWidth,
      }
  );
}

void Select::animateCaret(bool open) {
  const float to = open ? 1.0f : 0.0f;
  if (animationManager() != nullptr) {
    if (m_caretAnimId != 0) {
      animationManager()->cancel(m_caretAnimId);
    }
    m_caretAnimId = animationManager()->animate(
        m_caretProgress, to, Style::animNormal, Easing::EaseOutCubic,
        [this](float t) {
          m_caretProgress = t;
          applyVisualState();
          markPaintDirty();
        },
        [this]() { m_caretAnimId = 0; }, this
    );
    markPaintDirty();
  } else {
    m_caretProgress = to;
  }
}

void Select::syncTriggerText() {
  if (m_triggerLabel == nullptr) {
    return;
  }
  m_triggerLabel->setText(selectedText().empty() ? m_placeholder : std::string(selectedText()));
  m_triggerLabel->setFontSize(m_fontSize);
}

void Select::toggleOpen() {
  if (!m_enabled || m_options.empty()) {
    return;
  }
  if (m_open) {
    closeMenu();
  } else {
    openPopupDropdown();
  }
}

void Select::closeMenu() {
  if (!m_open) {
    return;
  }
  m_open = false;
  auto* ctx = popupContext();
  if (ctx != nullptr && ctx->isSelectDropdownOpen()) {
    ctx->closeSelectDropdown();
  }
  animateCaret(false);
}

void Select::openPopupDropdown() {
  auto* ctx = popupContext();
  if (ctx == nullptr) {
    return;
  }

  float absLeft = 0.0f;
  float absTop = 0.0f;
  float absRight = 0.0f;
  float absBottom = 0.0f;
  Node::transformedBounds(this, absLeft, absTop, absRight, absBottom);

  const float triggerWidth = absRight - absLeft;
  const float triggerHeight = absBottom - absTop;

  // Compute menu width: match trigger width, but also consider widest option label
  float menuWidth = std::max(minWidth(), triggerWidth);

  SelectPopupContext::DropdownRequest request{
      .anchorX = static_cast<std::int32_t>(std::round(absLeft)),
      .anchorY = static_cast<std::int32_t>(std::round(absTop)),
      .anchorWidth = static_cast<std::int32_t>(std::round(triggerWidth)),
      .anchorHeight = static_cast<std::int32_t>(std::round(triggerHeight)),
      .menuWidth = menuWidth,
      .optionHeight = m_controlHeight,
      .fontSize = m_fontSize,
      .glyphSize = m_glyphSize,
      .horizontalPadding = m_horizontalPadding,
      .options = m_options,
      .indicatorColors = m_indicatorColors,
      .optionSwatchPreviews = m_optionSwatchPreviews,
      .selectedIndex = m_selectedIndex,
      .maxVisibleOptions = static_cast<std::size_t>(kMaxVisibleOptions),
  };

  SelectPopupContext::DropdownCallbacks callbacks{
      .onSelect =
          [this](std::size_t index) {
            m_open = false;
            animateCaret(false);
            setSelectedIndex(index);
          },
      .onDismiss =
          [this]() {
            m_open = false;
            animateCaret(false);
          },
      .onHoverChanged = nullptr,
  };

  m_open = true;
  animateCaret(true);
  ctx->openSelectDropdown(request, std::move(callbacks));
}
