#include "shell/bar/widgets/clock_widget.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace {
  constexpr float kStackedPrimaryScale = 0.72f;
  constexpr float kStackedSecondaryScale = 0.62f;
  constexpr float kStackedPrimaryScaleNoCapsule = 0.80f;
  constexpr float kStackedSecondaryScaleNoCapsule = 0.72f;

  std::pair<std::string_view, std::string_view> splitFirstLine(std::string_view text) {
    const std::size_t newline = text.find('\n');
    if (newline == std::string_view::npos) {
      return {text, {}};
    }
    return {text.substr(0, newline), text.substr(newline + 1)};
  }
} // namespace

ClockWidget::ClockWidget(wl_output* /*output*/, std::string format, std::string verticalFormat)
    : m_format(std::move(format)), m_verticalFormat(std::move(verticalFormat)) {}

std::string ClockWidget::formatTimeText() const {
  if (!m_isVertical) {
    return formatLocalTime(m_format.c_str());
  }

  if (!m_verticalFormat.empty()) {
    return formatLocalTime(m_verticalFormat.c_str());
  }

  // Fallback for vertical bars when no explicit vertical_format is configured:
  // stack each whitespace- or colon-separated token on its own line so "21:15"
  // splits into "21" / "15". Matches Pango's lineBudget (1 + '\n' count) so
  // nothing gets ellipsized unless a single token is wider than the bar.
  auto text = formatLocalTime(m_format.c_str());
  std::string out;
  out.reserve(text.size());
  bool lastWasBreak = true;
  for (char c : text) {
    const bool isBreak = (c == ' ' || c == '\t' || c == ':');
    if (isBreak) {
      if (!lastWasBreak) {
        out.push_back('\n');
        lastWasBreak = true;
      }
    } else {
      out.push_back(c);
      lastWasBreak = false;
    }
  }
  if (!out.empty() && out.back() == '\n') {
    out.pop_back();
  }
  return out;
}

void ClockWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    requestPanelToggle("control-center", "calendar");
  });

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .textAlign = TextAlign::Center,
      })
  );

  area->addChild(
      ui::label({
          .out = &m_secondaryLabel,
          .fontSize = Style::fontSizeBody * m_contentScale * kStackedSecondaryScale,
          .fontWeight = labelFontWeight(),
          .textAlign = TextAlign::Center,
          .visible = false,
      })
  );

  setRoot(std::move(area));
}

void ClockWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (m_label == nullptr || m_secondaryLabel == nullptr || rootNode == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  update(renderer);
  const ColorSpec foreground = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
  m_label->setColor(foreground);
  m_secondaryLabel->setColor(foreground);

  const bool showSecondary = !m_isVertical && !m_lastSecondaryText.empty();
  const bool noCapsule = !barCapsuleSpec().enabled;
  const float stackedPrimaryScale = noCapsule ? kStackedPrimaryScaleNoCapsule : kStackedPrimaryScale;
  const float stackedSecondaryScale = noCapsule ? kStackedSecondaryScaleNoCapsule : kStackedSecondaryScale;
  float primaryFontSize = Style::fontSizeBody * m_contentScale * (showSecondary ? stackedPrimaryScale : 1.0f);
  float secondaryFontSize = Style::fontSizeBody * m_contentScale * stackedSecondaryScale;
  const FontWeight fontWeight = labelFontWeight();

  // Horizontal clocks use single-line metrics unless the configured format
  // explicitly contains line breaks.
  m_label->setFontSize(primaryFontSize);
  m_label->setFontWeight(fontWeight);
  m_secondaryLabel->setFontWeight(fontWeight);
  m_secondaryLabel->setFontSize(secondaryFontSize);
  m_label->setMaxLines(m_isVertical ? 0 : 1);
  m_label->setMinWidth(0.0f);
  m_label->setMaxWidth(m_isVertical ? containerWidth : 0.0f);
  m_label->measure(renderer);

  m_secondaryLabel->setVisible(showSecondary);
  m_secondaryLabel->setMaxLines(0);
  m_secondaryLabel->setMinWidth(0.0f);
  m_secondaryLabel->setMaxWidth(0.0f);
  if (showSecondary) {
    m_secondaryLabel->measure(renderer);
  }

  float width = showSecondary ? std::max(m_label->width(), m_secondaryLabel->width()) : m_label->width();
  float height = showSecondary ? m_label->height() + m_secondaryLabel->height() : m_label->height();
  if (!m_isVertical && showSecondary && containerHeight > 0.0f && height > containerHeight) {
    const float fitScale = std::min(containerHeight / height, 1.0f);
    primaryFontSize *= fitScale;
    secondaryFontSize *= fitScale;
    m_label->setFontSize(primaryFontSize);
    m_secondaryLabel->setFontSize(secondaryFontSize);
    m_label->measure(renderer);
    m_secondaryLabel->measure(renderer);
    width = std::max(m_label->width(), m_secondaryLabel->width());
    height = m_label->height() + m_secondaryLabel->height();
  }

  if (showSecondary) {
    const auto primaryMetrics =
        renderer.measureText(m_lastPrimaryText, primaryFontSize, fontWeight, 0.0f, 1, TextAlign::Start);
    const auto secondaryMetrics =
        renderer.measureText(m_lastSecondaryText, secondaryFontSize, fontWeight, 0.0f, 1, TextAlign::Start);
    const float primaryInkWidth = std::max(0.0f, primaryMetrics.inkRight - primaryMetrics.inkLeft);
    const float secondaryInkWidth = std::max(0.0f, secondaryMetrics.inkRight - secondaryMetrics.inkLeft);
    width = std::max({width, primaryInkWidth, secondaryInkWidth});
    const float centerX = width * 0.5f;
    const float primaryInkCenterX = (primaryMetrics.inkLeft + primaryMetrics.inkRight) * 0.5f;
    const float secondaryInkCenterX = (secondaryMetrics.inkLeft + secondaryMetrics.inkRight) * 0.5f;
    m_label->setPosition(std::round(centerX - primaryInkCenterX), 0.0f);
    m_secondaryLabel->setPosition(std::round(centerX - secondaryInkCenterX), m_label->height());
  } else {
    m_label->setPosition(0.0f, 0.0f);
  }
  rootNode->setSize(width, height);
}

void ClockWidget::doUpdate(Renderer& renderer) {
  if (m_label == nullptr || m_secondaryLabel == nullptr) {
    return;
  }

  auto text = formatTimeText();
  if (text != m_lastText) {
    m_lastText = std::move(text);
  }

  std::string primaryText = m_lastText;
  std::string secondaryText;
  if (!m_isVertical) {
    const auto [primary, secondary] = splitFirstLine(m_lastText);
    primaryText = std::string(primary);
    secondaryText = std::string(secondary);
  }

  if (primaryText != m_lastPrimaryText) {
    m_lastPrimaryText = std::move(primaryText);
    m_label->setText(m_lastPrimaryText);
    m_label->measure(renderer);
  }
  if (secondaryText != m_lastSecondaryText) {
    m_lastSecondaryText = std::move(secondaryText);
    m_secondaryLabel->setText(m_lastSecondaryText);
    m_secondaryLabel->measure(renderer);
  }
}
