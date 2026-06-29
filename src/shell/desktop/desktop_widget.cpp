#include "shell/desktop/desktop_widget.h"

#include "core/ui_phase.h"
#include "ui/controls/box.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace {
  constexpr float kMinContentFit = 0.05f;
  constexpr float kMaxContentFit = 20.0f;
} // namespace

float DesktopWidget::boxInnerWidth() const noexcept {
  if (m_boxWidth <= 0.0f) {
    return 0.0f;
  }
  const float pad = m_bgEnabled ? std::round(m_bgPadding * m_baseScale) : 0.0f;
  return std::max(1.0f, m_boxWidth - 2.0f * pad);
}

float DesktopWidget::boxInnerHeight() const noexcept {
  if (m_boxHeight <= 0.0f) {
    return 0.0f;
  }
  const float pad = m_bgEnabled ? std::round(m_bgPadding * m_baseScale) : 0.0f;
  return std::max(1.0f, m_boxHeight - 2.0f * pad);
}

void DesktopWidget::layout(Renderer& renderer) {
  UiPhaseScope layoutPhase(UiPhase::Layout);
  m_inLayout = true;
  struct InLayoutReset {
    bool& flag;
    ~InLayoutReset() { flag = false; }
  } inLayoutReset{m_inLayout};

  // Apply the configured font before measuring so text metrics are correct on the first pass and
  // the family survives widget rebuilds (the factory only stores it on the widget).
  onFontFamilyChanged(m_fontFamily, renderer);

  // First pass measures the content at its natural (base) scale.
  m_contentScale = m_baseScale;
  doLayout(renderer);

  // When the tile has an explicit box, scale the content to fill it (aspect-preserved) and
  // re-lay out so text/glyphs are rasterized crisp at the fitted scale. The fit tracks the widest
  // content seen (high-water mark) so dynamic text keeps a stable font instead of breathing.
  if (m_boxWidth > 0.0f && m_boxHeight > 0.0f && m_contentRoot != nullptr) {
    if (m_fitRefScale != m_baseScale) {
      m_maxNaturalWidth = 0.0f;
      m_maxNaturalHeight = 0.0f;
      m_fitRefScale = m_baseScale;
    }
    m_maxNaturalWidth = std::max({m_maxNaturalWidth, 1.0f, m_contentRoot->width()});
    m_maxNaturalHeight = std::max({m_maxNaturalHeight, 1.0f, m_contentRoot->height()});

    const float fitted = contentScaleForBox(m_boxWidth, m_boxHeight);
    if (std::abs(fitted - m_baseScale) > 0.001f * m_baseScale) {
      m_contentScale = fitted;
      doLayout(renderer);
    }
  }

  applyBackground();
}

float DesktopWidget::contentScaleForBox(float boxWidth, float boxHeight) const noexcept {
  if (boxWidth <= 0.0f || boxHeight <= 0.0f || m_maxNaturalWidth <= 0.0f || m_maxNaturalHeight <= 0.0f) {
    return m_contentScale;
  }
  const float pad = m_bgEnabled ? std::round(m_bgPadding * m_baseScale) : 0.0f;
  const float innerW = std::max(1.0f, boxWidth - 2.0f * pad);
  const float innerH = std::max(1.0f, boxHeight - 2.0f * pad);
  const float fit =
      std::clamp(std::min(innerW / m_maxNaturalWidth, innerH / m_maxNaturalHeight), kMinContentFit, kMaxContentFit);
  return m_baseScale * fit;
}

void DesktopWidget::update(Renderer& renderer) {
  UiPhaseScope updatePhase(UiPhase::Update);
  doUpdate(renderer);
}

Node* DesktopWidget::presentationRoot() const noexcept {
  if (m_outerRootPtr != nullptr) {
    return m_outerRootPtr;
  }
  if (m_outerRoot != nullptr) {
    return m_outerRoot.get();
  }
  return m_contentRoot;
}

bool DesktopWidget::hasVisibleBackground() const noexcept {
  if (!m_bgEnabled) {
    return false;
  }
  const Node* node = presentationRoot();
  return node != nullptr && node->visible() && node->opacity() > 0.001f;
}

float DesktopWidget::intrinsicWidth() const noexcept {
  if (m_boxWidth > 0.0f) {
    return m_boxWidth;
  }
  if (m_contentRoot == nullptr) {
    return 0.0f;
  }
  float w = m_contentRoot->width();
  if (m_bgEnabled) {
    w += 2.0f * std::round(m_bgPadding * m_baseScale);
  }
  return w;
}

float DesktopWidget::intrinsicHeight() const noexcept {
  if (m_boxHeight > 0.0f) {
    return m_boxHeight;
  }
  if (m_contentRoot == nullptr) {
    return 0.0f;
  }
  float h = m_contentRoot->height();
  if (m_bgEnabled) {
    h += 2.0f * std::round(m_bgPadding * m_baseScale);
  }
  return h;
}

std::unique_ptr<Node> DesktopWidget::releaseRoot() {
  if (m_outerRoot) {
    m_outerRootPtr = m_outerRoot.get();
    return std::move(m_outerRoot);
  }
  return nullptr;
}

void DesktopWidget::setRoot(std::unique_ptr<Node> root) {
  // Content is always wrapped in an outer node so the box tile can be larger than the
  // content (content is centered inside it), with or without a background.
  m_contentRoot = root.get();
  m_outerRoot = std::make_unique<Node>();
  if (m_bgEnabled) {
    auto bg = std::make_unique<Box>();
    m_bgBox = bg.get();
    m_outerRoot->addChild(std::move(bg));
  }
  m_outerRoot->addChild(std::move(root));
}

void DesktopWidget::setBackgroundStyle(const ColorSpec& color, float radius, float padding) {
  m_bgEnabled = true;
  m_bgColor = color;
  m_bgRadius = radius;
  m_bgPadding = padding;
}

bool DesktopWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "font_family") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_fontFamily = *v;
      onFontFamilyChanged(m_fontFamily, renderer);
      requestLayout();
      return true;
    }
    return false;
  }
  if (key != "background_color"
      && key != "background_opacity"
      && key != "background_radius"
      && key != "background_padding") {
    return false;
  }
  if (!m_bgEnabled || m_bgBox == nullptr) {
    return false;
  }

  auto getFloat = [&](const std::string& k, float fb) -> float {
    auto it = allSettings.find(k);
    if (it == allSettings.end())
      return fb;
    if (const auto* v = std::get_if<double>(&it->second))
      return static_cast<float>(*v);
    if (const auto* v = std::get_if<std::int64_t>(&it->second))
      return static_cast<float>(*v);
    return fb;
  };
  auto getColorSpec = [&](const std::string& k, const ColorSpec& fb) -> ColorSpec {
    auto it = allSettings.find(k);
    if (it == allSettings.end())
      return fb;
    if (const auto* v = std::get_if<std::string>(&it->second))
      return colorSpecFromConfigString(*v, k);
    return fb;
  };

  ColorSpec bgColor = getColorSpec("background_color", colorSpecFromRole(ColorRole::Surface));
  bgColor.alpha *= std::clamp(getFloat("background_opacity", 0.8f), 0.0f, 1.0f);
  m_bgColor = bgColor;
  m_bgRadius = getFloat("background_radius", 12.0f);
  m_bgPadding = getFloat("background_padding", 10.0f);

  layout(renderer);
  return true;
}

void DesktopWidget::applyBackground() {
  if (m_contentRoot == nullptr) {
    return;
  }

  const bool boxed = m_boxWidth > 0.0f && m_boxHeight > 0.0f;
  const float pad = m_bgEnabled ? std::round(m_bgPadding * m_baseScale) : 0.0f;
  const float contentW = m_contentRoot->width();
  const float contentH = m_contentRoot->height();
  const float boxW = boxed ? m_boxWidth : contentW + 2.0f * pad;
  const float boxH = boxed ? m_boxHeight : contentH + 2.0f * pad;

  // Center the content inside the tile.
  m_contentRoot->setPosition(std::round((boxW - contentW) * 0.5f), std::round((boxH - contentH) * 0.5f));

  if (m_bgEnabled && m_bgBox != nullptr) {
    m_bgBox->setPosition(0.0f, 0.0f);
    m_bgBox->setSize(boxW, boxH);
    m_bgBox->setFill(m_bgColor);
    m_bgBox->setRadius(std::round(m_bgRadius * m_baseScale));
  }

  Node* outerRoot = m_outerRoot ? m_outerRoot.get() : m_outerRootPtr;
  if (outerRoot != nullptr) {
    outerRoot->setSize(boxW, boxH);
    outerRoot->setClipChildren(m_bgEnabled || boxed);
  }
}
