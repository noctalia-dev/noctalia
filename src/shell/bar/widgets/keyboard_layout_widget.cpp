#include "shell/bar/widgets/keyboard_layout_widget.h"

#include "compositors/compositor_platform.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/keyboard_layout_label.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

  constexpr auto kRefreshTickInterval = std::chrono::milliseconds(40);
  constexpr int kRefreshBurstAttempts = 8;
  constexpr std::string_view kVerticalStableLabel = "WWW";

} // namespace

KeyboardLayoutWidget::KeyboardLayoutWidget(
    CompositorPlatform& platform, Options options, std::unordered_map<std::string, std::string> customLabels
)
    : m_platform(platform), m_displayMode(options.display), m_showGlyph(options.showGlyph),
      m_showLabel(options.showLabel), m_hideWhenSingleLayout(options.hideWhenSingleLayout),
      m_customLabels(std::move(customLabels)), m_glyphName(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)) {
  if (m_glyphName.empty()) {
    m_glyphName = "keyboard";
  }
}

void KeyboardLayoutWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_glyphName,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  area->addChild(
      ui::label({
          .out = &m_label,
          .text = "--",
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
      })
  );

  setRoot(std::move(area));

  // The bar's normal second tick refreshes passive compositor-side layout changes.
  // Click-initiated switches still use the short burst timer below for responsive feedback.
}

void KeyboardLayoutWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_label == nullptr || root() == nullptr) {
    return;
  }

  m_isVertical = containerHeight > containerWidth;
  sync(renderer);
  if (!root()->visible()) {
    root()->setSize(0.0F, 0.0F);
    return;
  }

  const bool showGlyph = m_showGlyph && (m_image != nullptr || m_glyph != nullptr);
  if (m_image != nullptr) {
    m_image->setVisible(m_showGlyph);
  }
  if (m_glyph != nullptr) {
    m_glyph->setVisible(m_showGlyph);
  }
  if (showGlyph) {
    if (m_image != nullptr) {
      widget_custom_image::sync(
          *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
      );
    } else {
      m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
      m_glyph->measure(renderer);
    }
    if (m_glyph != nullptr && m_glyph->width() <= 0.0F && m_glyphName == "keyboard") {
      // Some fonts may miss the keyboard glyph; use a guaranteed fallback.
      m_glyph->setGlyph("world");
      m_glyph->measure(renderer);
    }
  }

  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->setVisible(m_showLabel);
  m_label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
  if (m_showLabel) {
    const float stableLabelWidth = std::round(renderer
                                                  .measureText(
                                                      kVerticalStableLabel, m_label->fontSize(), labelFontWeight(),
                                                      0.0F, 0, TextAlign::Start, labelFontFamily()
                                                  )
                                                  .width);
    m_label->setMinWidth(m_isVertical ? std::min(containerWidth, stableLabelWidth) : 0.0F);
    m_label->measure(renderer);
  }

  if (m_isVertical) {
    const float glyphW = showGlyph ? (m_image != nullptr ? m_image->width() : m_glyph->width()) : 0.0F;
    const float glyphH = showGlyph ? (m_image != nullptr ? m_image->height() : m_glyph->height()) : 0.0F;
    const float labelW = m_showLabel ? m_label->width() : 0.0F;
    const float labelH = m_showLabel ? m_label->height() : 0.0F;
    const float w = std::max(glyphW, labelW);
    float y = 0.0F;
    if (showGlyph) {
      if (m_image != nullptr) {
        m_image->setPosition(std::round((w - glyphW) * 0.5F), y);
      } else {
        m_glyph->setPosition(std::round((w - glyphW) * 0.5F), y);
      }
      y += glyphH;
    }
    if (m_showLabel) {
      m_label->setPosition(std::round((w - labelW) * 0.5F), y);
      y += labelH;
    }
    root()->setSize(w, y);
  } else {
    const float spacing = Style::spaceXs;
    float x = 0.0F;
    const float glyphH = showGlyph ? (m_image != nullptr ? m_image->height() : m_glyph->height()) : 0.0F;
    const float labelH = m_showLabel ? m_label->height() : 0.0F;
    const float h = std::max(glyphH, labelH);
    if (showGlyph) {
      if (m_image != nullptr) {
        const float imageY = std::round((h - m_image->height()) * 0.5F);
        m_image->setPosition(0.0F, imageY);
        x += m_image->width();
      } else {
        const float glyphY = std::round((h - m_glyph->height()) * 0.5F);
        m_glyph->setPosition(0.0F, glyphY);
        x += m_glyph->width();
      }
      if (m_showLabel) {
        x += spacing;
      }
    }
    if (m_showLabel) {
      const float labelY = std::round((h - m_label->height()) * 0.5F);
      m_label->setPosition(x, labelY);
      root()->setSize(m_label->x() + m_label->width(), h);
    } else {
      root()->setSize(x, h);
    }
  }
}

void KeyboardLayoutWidget::doUpdate(Renderer& renderer) { sync(renderer); }

std::string KeyboardLayoutWidget::resolvedLayoutName() const {
  const auto state = m_platform.keyboardLayoutState();
  if (state.has_value() && state->currentIndex >= 0 && state->currentIndex < static_cast<int>(state->names.size())) {
    const std::string actual = state->names[static_cast<std::size_t>(state->currentIndex)];
    if (!m_pendingLayoutName.empty() && (actual.empty() || actual == m_lastLayoutName)) {
      return m_pendingLayoutName;
    }
    return actual;
  }

  std::string layoutName = m_platform.currentKeyboardLayoutName();
  if (!m_pendingLayoutName.empty() && (layoutName.empty() || layoutName == m_lastLayoutName)) {
    return m_pendingLayoutName;
  }
  if (!layoutName.empty()) {
    return layoutName;
  }

  return m_pendingLayoutName;
}

void KeyboardLayoutWidget::sync(Renderer& renderer) {
  if (m_label == nullptr) {
    return;
  }

  if (auto* node = root(); node != nullptr) {
    const auto layoutNames = m_platform.keyboardLayoutNames();
    const bool shouldHide = m_hideWhenSingleLayout && !layoutNames.empty() && layoutNames.size() <= 1;
    node->setVisible(!shouldHide);
    if (shouldHide) {
      node->setSize(0.0F, 0.0F);
      requestRedraw();
      return;
    }
  }

  std::string layoutName = resolvedLayoutName();
  if (!m_pendingLayoutName.empty() && layoutName == m_pendingLayoutName) {
    m_pendingLayoutName.clear();
    m_refreshAttemptsRemaining = 0;
    m_refreshTimer.stop();
  }
  std::string layoutLabel = resolveKeyboardLayoutLabel(layoutName, m_displayMode, m_customLabels);
  if (m_isVertical) {
    layoutLabel = StringUtils::truncateUtf8CodePoints(layoutLabel, 3);
  }

  if (layoutName == m_lastLayoutName && layoutLabel == m_lastLabel && m_isVertical == m_lastVertical) {
    return;
  }

  m_lastLayoutName = layoutName;
  m_lastLabel = layoutLabel;
  m_lastVertical = m_isVertical;

  if (m_image != nullptr) {
    m_image->setVisible(m_showGlyph);
  } else if (m_glyph != nullptr) {
    m_glyph->setVisible(m_showGlyph);
  }
  m_label->setVisible(m_showLabel);
  if (m_showLabel) {
    m_label->setFontSize((m_isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setText(layoutLabel);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    if (m_showLabel) {
      area->clearTooltip();
    } else {
      const std::string tooltipText = layoutName.empty() ? layoutLabel : layoutName;
      area->setTooltip(tooltipText);
    }
  }

  if (auto* node = root(); node != nullptr) {
    // Dim only when the click cannot do anything: the compositor backend is missing and the left
    // binding is still the one that needs it.
    const auto* left = gestureBindings().find(noctalia::bar::Gesture::Left);
    const bool needsBackend = left != nullptr
        && left->kind == noctalia::bar::WidgetAction::Kind::Ipc
        && left->verb == "keyboard-layout-cycle";
    node->setOpacity(needsBackend && !m_platform.hasKeyboardLayoutBackend() ? 0.85F : 1.0F);
  }

  requestRedraw();
}

// The compositor reports the new layout on its own schedule, so show the layout the cycle is
// about to land on and poll hard until reality catches up.
void KeyboardLayoutWidget::onGestureDispatch(
    noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action
) {
  (void)gesture;
  if (action.kind != noctalia::bar::WidgetAction::Kind::Ipc || action.verb != "keyboard-layout-cycle") {
    return;
  }

  const auto stateBefore = m_platform.keyboardLayoutState();
  if (stateBefore.has_value()
      && stateBefore->currentIndex >= 0
      && stateBefore->currentIndex < static_cast<int>(stateBefore->names.size())
      && stateBefore->names.size() > 1) {
    auto nextIndex = static_cast<std::size_t>(stateBefore->currentIndex + 1);
    if (nextIndex >= stateBefore->names.size()) {
      nextIndex = 0;
    }
    m_pendingLayoutName = stateBefore->names[nextIndex];
  }

  scheduleRefreshBurst();
  requestUpdate();
}

void KeyboardLayoutWidget::scheduleRefreshBurst() {
  m_refreshAttemptsRemaining = kRefreshBurstAttempts;
  armRefreshTick();
}

void KeyboardLayoutWidget::armRefreshTick() {
  m_refreshTimer.start(kRefreshTickInterval, [this]() {
    if (m_refreshAttemptsRemaining <= 0) {
      m_pendingLayoutName.clear();
      m_refreshTimer.stop();
      requestUpdate();
      return;
    }

    --m_refreshAttemptsRemaining;
    if (m_refreshAttemptsRemaining > 0) {
      armRefreshTick();
    } else {
      m_pendingLayoutName.clear();
      m_refreshTimer.stop();
    }

    requestUpdate();
  });
}
