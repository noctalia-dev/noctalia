#include "shell/bar/widgets/workspaces_widget.h"

#include "core/ui_phase.h"
#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  [[nodiscard]] bool isEmptyWorkspace(const Workspace& workspace) {
    return !workspace.occupied && !workspace.active && !workspace.urgent;
  }

  constexpr float kWorkspaceGap = Style::spaceXs;
  constexpr float kWorkspacePillDefaultHeight = Style::baseGlyphSize;
  constexpr float kWorkspaceAnimDurationMs = static_cast<float>(Style::animNormal);

  [[nodiscard]] FontWeight workspaceFontWeight(FontWeight baseWeight, bool minimal, bool active) {
    if (minimal && active) {
      return static_cast<FontWeight>(static_cast<int>(baseWeight) + 200);
    }
    return baseWeight;
  }

  // Numeric workspace IDs ("10", "11") must not be truncated like word labels.
  [[nodiscard]] bool isNumericLabel(std::string_view label) {
    return !label.empty()
        && std::ranges::all_of(label, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
  }
} // namespace

WorkspacesWidget::WorkspacesWidget(CompositorPlatform& platform, wl_output* output, Options options)
    : m_platform(platform), m_output(output), m_displayMode(options.displayMode),
      m_maxLabelChars(options.maxLabelChars), m_labelsOnlyWhenOccupied(options.labelsOnlyWhenOccupied),
      m_hideWhenEmpty(options.hideWhenEmpty), m_pillScale(options.pillScale),
      m_activePillSize(std::clamp(options.activePillSize, 0.25f, 8.0f)),
      m_inactivePillSize(std::clamp(options.inactivePillSize, 0.25f, 8.0f)), m_minimal(options.minimal),
      m_focusedColor(options.focusedColor), m_occupiedColor(options.occupiedColor), m_emptyColor(options.emptyColor) {}

WorkspacesWidget::DisplayMode WorkspacesWidget::effectiveDisplayMode() const noexcept {
  if (m_minimal && m_displayMode == DisplayMode::None) {
    return DisplayMode::Id;
  }
  return m_displayMode;
}

bool WorkspacesWidget::shouldShowWorkspaceLabel(const Workspace& workspace, std::string_view label) const noexcept {
  if (effectiveDisplayMode() == DisplayMode::None || label.empty()) {
    return false;
  }
  if (m_labelsOnlyWhenOccupied && !workspace.occupied && !workspace.active) {
    return false;
  }
  return true;
}

bool WorkspacesWidget::isWorkspaceHidden(const Workspace& workspace) const noexcept {
  return m_hideWhenEmpty && isEmptyWorkspace(workspace);
}

void WorkspacesWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f) {
      return;
    }
    // Wayland reports positive wheel deltas for "scroll down", so treat that
    // as moving to the next workspace and negative as previous.
    activateAdjacentWorkspace(delta > 0.0f ? 1 : -1);
  });
  m_container = container.get();
  setRoot(std::move(container));
}

void WorkspacesWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  const bool wasVertical = m_isVertical;
  m_isVertical = containerHeight > containerWidth;
  if (wasVertical != m_isVertical) {
    m_rebuildPending = true;
  }
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_textMetricsGeneration != textMetricsGeneration) {
    m_textMetricsGeneration = textMetricsGeneration;
    m_rebuildPending = true;
  }
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  }
}

void WorkspacesWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }
}

void WorkspacesWidget::doUpdate(Renderer& renderer) {
  auto current = m_platform.workspaces(m_output);

  if (!m_cachedState.empty() && !current.empty() && !std::ranges::any_of(current, [](const Workspace& ws) {
        return ws.active;
      })) {
    return;
  }

  const bool showWidget = !current.empty()
      && (!m_hideWhenEmpty || std::ranges::any_of(current, [](const Workspace& ws) { return !isEmptyWorkspace(ws); }));
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    if (!m_cachedState.empty() || !m_items.empty()) {
      m_cachedState.clear();
      m_rebuildPending = true;
      if (root() != nullptr) {
        root()->markLayoutDirty();
      }
    }
    return;
  }

  if (m_cachedState.empty() && current.empty()) {
    return;
  }

  bool structuralChange = current.size() != m_cachedState.size();
  bool activeChange = false;
  if (!structuralChange) {
    for (std::size_t i = 0; i < current.size(); ++i) {
      const auto& a = current[i];
      const auto& b = m_cachedState[i];
      if (a.id != b.id || a.name != b.name || a.index != b.index || a.coordinates != b.coordinates) {
        structuralChange = true;
        break;
      }
      if (a.active != b.active || a.urgent != b.urgent) {
        activeChange = true;
      }
      if (a.occupied != b.occupied) {
        activeChange = true;
      }
    }
  }

  if (!structuralChange && !activeChange) {
    return;
  }

  m_cachedState.clear();
  m_cachedState.reserve(current.size());
  for (const auto& ws : current) {
    m_cachedState.push_back(
        Workspace{
            .id = ws.id,
            .name = ws.name,
            .coordinates = ws.coordinates,
            .index = ws.index,
            .active = ws.active,
            .urgent = ws.urgent,
            .occupied = ws.occupied
        }
    );
  }

  if (structuralChange) {
    m_rebuildPending = true;
    if (root() != nullptr) {
      root()->markLayoutDirty();
    }
  } else {
    retarget(renderer);
  }
}

void WorkspacesWidget::rebuild(Renderer& renderer) {
  uiAssertNotRendering("WorkspacesWidget::rebuild");
  cancelAnimation();
  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }
  m_items.clear();

  const auto& workspaces = m_cachedState;
  const float gap = kWorkspaceGap * m_contentScale;
  const float labelFontSize = Style::fontSizeMini * m_contentScale;
  const float pillHeight = std::round(kWorkspacePillDefaultHeight * m_contentScale * m_pillScale);
  const FontWeight configuredFontWeight = labelFontWeight();

  std::vector<std::string> labels;
  labels.reserve(workspaces.size());
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    labels.push_back(workspaceLabel(workspaces[i], i));
  }

  // Measure text and compute per-slot widths along the bar main axis.
  // Width = max(baseSize * pill_size, textWidth + padding); pill_size comes from active/inactive settings.
  struct SlotMetrics {
    std::string label;
    bool showLabel = false;
    float textWidth = 0.0f;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
  };
  std::vector<SlotMetrics> slots(workspaces.size());

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
    slot.label = labels[i];
    slot.showLabel = shouldShowWorkspaceLabel(workspaces[i], labels[i]);

    if (slot.showLabel) {
      const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspaces[i].active);
      const TextMetrics tm = renderer.measureText(labels[i], labelFontSize, slotFontWeight);
      slot.textWidth = std::max(tm.right - tm.left, tm.inkRight - tm.inkLeft);
    }
  }

  const float baseSize = std::round(pillHeight);
  const float padding = m_minimal ? (Style::spaceXs * m_contentScale) : (baseSize * 0.6f);
  float maxLabelHeight = labelFontSize;

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
    if (isWorkspaceHidden(workspaces[i])) {
      slot.showLabel = false;
      slot.inactiveWidth = 0.0f;
      slot.activeWidth = 0.0f;
      continue;
    }

    if (m_minimal) {
      const float minWidth = baseSize;
      if (!slot.showLabel) {
        slot.inactiveWidth = minWidth;
        slot.activeWidth = minWidth;
      } else {
        const float textBasedWidth = slot.textWidth + padding * 2.0f;
        slot.inactiveWidth = std::max(minWidth, textBasedWidth);
        slot.activeWidth = slot.inactiveWidth;
      }
      if (slot.showLabel) {
        const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspaces[i].active);
        const TextMetrics tm = renderer.measureText(slot.label, labelFontSize, slotFontWeight);
        maxLabelHeight = std::max(maxLabelHeight, tm.bottom - tm.top);
      }
      continue;
    }

    const float minWidth = workspaceMainAxisMinWidth(baseSize, false);
    const float minActiveWidth = workspaceMainAxisMinWidth(baseSize, true);

    if (!slot.showLabel) {
      slot.inactiveWidth = minWidth;
      slot.activeWidth = minActiveWidth;
    } else {
      const float textBasedWidth = slot.textWidth + padding;
      slot.inactiveWidth = std::max(minWidth, textBasedWidth);
      slot.activeWidth = std::max(minActiveWidth, textBasedWidth);
    }
  }

  m_gap = gap;
  m_indicatorHeight = m_minimal ? std::round(maxLabelHeight + padding) : pillHeight;

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const auto& ws = workspaces[i];
    const auto& slot = slots[i];

    auto area = std::make_unique<InputArea>();
    const float w = ws.active ? slot.activeWidth : slot.inactiveWidth;
    area->setFrameSize(w, m_indicatorHeight);

    Item item{};
    item.active = ws.active;
    item.label = slot.label;
    item.showLabel = slot.showLabel;
    item.inactiveWidth = slot.inactiveWidth;
    item.activeWidth = slot.activeWidth;

    if (!m_minimal) {
      const float indicatorW = m_isVertical ? m_indicatorHeight : w;
      const float indicatorH = m_isVertical ? w : m_indicatorHeight;
      item.indicator = static_cast<Box*>(area->addChild(
          ui::box({
              .fill = workspaceFillColor(ws),
              .radius = workspacePillRadius(indicatorW, indicatorH),
              .width = w,
              .height = m_indicatorHeight,
              .configure = [](Box& box) { box.clearBorder(); },
          })
      ));
    }

    if (slot.showLabel) {
      item.text = static_cast<Label*>(area->addChild(
          ui::label({
              .text = slot.label,
              .fontSize = labelFontSize,
              .fontFamily = labelFontFamily(),
              .color = workspaceTextColor(ws),
              .fontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, ws.active),
              .baselineMode = LabelBaselineMode::StableLogical,
          })
      ));
      item.text->measure(renderer);
    }

    auto wsCopy = ws;
    area->setOnClick([this, wsCopy](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        m_platform.activateWorkspace(m_output, wsCopy);
      }
    });
    item.area = static_cast<InputArea*>(m_container->addChild(std::move(area)));
    m_items.push_back(item);
  }

  // Size the container after targets are known.
  computeTargets();
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    it.currentX = it.targetX;
    it.currentWidth = it.targetWidth;
    applyItemLayout(i);
  }

  float total = 0.0f;
  for (const auto& item : m_items) {
    total = std::max(total, item.currentX + item.currentWidth);
  }
  if (m_isVertical) {
    m_container->setFrameSize(m_indicatorHeight, total);
  } else {
    m_container->setFrameSize(total, m_indicatorHeight);
  }
}

void WorkspacesWidget::computeTargets() {
  float cursor = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const bool hidden = isWorkspaceHidden(m_cachedState[i]);
    const float w = hidden ? 0.0f : ((m_cachedState[i].active) ? it.activeWidth : it.inactiveWidth);
    it.targetX = cursor;
    it.targetWidth = w;
    it.active = m_cachedState[i].active;
    if (w > 0.0f) {
      cursor += w + m_gap;
    }
  }
}

void WorkspacesWidget::updateContainerSize() {
  if (m_container == nullptr || m_items.empty()) {
    return;
  }
  float total = 0.0f;
  for (const auto& item : m_items) {
    total = std::max(total, item.currentX + item.currentWidth);
  }
  if (m_isVertical) {
    m_container->setFrameSize(m_indicatorHeight, total);
  } else {
    m_container->setFrameSize(total, m_indicatorHeight);
  }
  if (Node* shell = barCapsuleShell(); shell != nullptr) {
    shell->markLayoutDirty();
  }
}

void WorkspacesWidget::ensureItemLabel(Renderer& renderer, std::size_t index) {
  if (index >= m_items.size() || index >= m_cachedState.size()) {
    return;
  }

  auto& item = m_items[index];
  const auto& workspace = m_cachedState[index];
  if (!item.showLabel || item.area == nullptr) {
    return;
  }
  if (item.text != nullptr) {
    return;
  }

  const float labelFontSize = Style::fontSizeMini * m_contentScale;
  item.text = static_cast<Label*>(item.area->addChild(
      ui::label({
          .text = item.label,
          .fontSize = labelFontSize,
          .fontFamily = labelFontFamily(),
          .color = workspaceTextColor(workspace),
          .fontWeight = workspaceFontWeight(labelFontWeight(), m_minimal, workspace.active),
          .baselineMode = LabelBaselineMode::StableLogical,
      })
  ));
  item.text->measure(renderer);
}

void WorkspacesWidget::recalculateItemMetrics(Renderer& renderer, std::size_t index) {
  if (index >= m_items.size() || index >= m_cachedState.size()) {
    return;
  }

  auto& item = m_items[index];
  const auto& workspace = m_cachedState[index];
  const std::string label = workspaceLabel(workspace, index);
  const float labelFontSize = Style::fontSizeMini * m_contentScale;
  const float pillHeight = std::round(kWorkspacePillDefaultHeight * m_contentScale * m_pillScale);
  const float baseSize = std::round(pillHeight);
  const float padding = m_minimal ? (Style::spaceXs * m_contentScale) : (baseSize * 0.6f);
  const FontWeight configuredFontWeight = labelFontWeight();

  item.label = label;
  item.showLabel = shouldShowWorkspaceLabel(workspace, label);

  if (isWorkspaceHidden(workspace)) {
    item.inactiveWidth = 0.0f;
    item.activeWidth = 0.0f;
    if (item.text != nullptr) {
      item.text->setVisible(false);
    }
    return;
  }

  float textWidth = 0.0f;
  if (item.showLabel) {
    const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspace.active);
    const TextMetrics tm = renderer.measureText(label, labelFontSize, slotFontWeight);
    textWidth = std::max(tm.right - tm.left, tm.inkRight - tm.inkLeft);
  }

  if (m_minimal) {
    const float minWidth = baseSize;
    if (!item.showLabel) {
      item.inactiveWidth = minWidth;
      item.activeWidth = minWidth;
    } else {
      const float textBasedWidth = textWidth + padding * 2.0f;
      item.inactiveWidth = std::max(minWidth, textBasedWidth);
      item.activeWidth = item.inactiveWidth;
    }
  } else {
    const float minWidth = workspaceMainAxisMinWidth(baseSize, false);
    const float minActiveWidth = workspaceMainAxisMinWidth(baseSize, true);
    if (!item.showLabel) {
      item.inactiveWidth = minWidth;
      item.activeWidth = minActiveWidth;
    } else {
      const float textBasedWidth = textWidth + padding;
      item.inactiveWidth = std::max(minWidth, textBasedWidth);
      item.activeWidth = std::max(minActiveWidth, textBasedWidth);
    }
  }

  ensureItemLabel(renderer, index);
  if (item.text != nullptr) {
    item.text->setVisible(item.showLabel);
    if (item.showLabel) {
      item.text->setText(label);
      item.text->setFontWeight(workspaceFontWeight(configuredFontWeight, m_minimal, workspace.active));
      item.text->setColor(workspaceTextColor(workspace));
      item.text->measure(renderer);
    }
  }
}

void WorkspacesWidget::updateAllItemMetrics(Renderer& renderer) {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    recalculateItemMetrics(renderer, i);
  }
}

void WorkspacesWidget::retarget(Renderer& renderer) {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const auto& ws = m_cachedState[i];
    if (it.indicator != nullptr) {
      it.indicator->setFill(workspaceFillColor(ws));
      it.indicator->clearBorder();
    }
  }

  updateAllItemMetrics(renderer);

  if (m_minimal) {
    computeTargets();
    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto& it = m_items[i];
      it.currentX = it.targetX;
      it.currentWidth = it.targetWidth;
      applyItemLayout(i);
    }
    updateContainerSize();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    return;
  }

  for (auto& it : m_items) {
    it.fromX = it.currentX;
    it.fromWidth = it.currentWidth;
  }
  computeTargets();
  startAnimation();
}

void WorkspacesWidget::startAnimation() {
  auto* mgr = m_animations;
  if (mgr == nullptr) {
    for (std::size_t i = 0; i < m_items.size(); ++i) {
      auto& it = m_items[i];
      it.currentX = it.targetX;
      it.currentWidth = it.targetWidth;
      applyItemLayout(i);
    }
    updateContainerSize();
    return;
  }
  cancelAnimation();
  requestFrameTick();
  requestRedraw();
  m_animId = mgr->animate(
      0.0f, 1.0f, kWorkspaceAnimDurationMs, Easing::EaseOutCubic,
      [this](float t) {
        for (std::size_t i = 0; i < m_items.size(); ++i) {
          auto& it = m_items[i];
          it.currentX = it.fromX + (it.targetX - it.fromX) * t;
          it.currentWidth = it.fromWidth + (it.targetWidth - it.fromWidth) * t;
          applyItemLayout(i);
        }
        updateContainerSize();
        if (root() != nullptr) {
          root()->markPaintDirty();
        }
      },
      [this]() { m_animId = 0; }, this
  );
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
}

void WorkspacesWidget::cancelAnimation() {
  if (m_animId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_animId);
  }
  m_animId = 0;
}

void WorkspacesWidget::applyItemLayout(std::size_t i) {
  auto& it = m_items[i];
  if (it.area == nullptr) {
    return;
  }
  const bool hidden = i < m_cachedState.size() && isWorkspaceHidden(m_cachedState[i]);
  const bool visible = !hidden || it.currentWidth > 0.0f;
  it.area->setVisible(visible);
  it.area->setParticipatesInLayout(visible);
  if (m_isVertical) {
    it.area->setPosition(0.0f, std::round(it.currentX));
    it.area->setFrameSize(m_indicatorHeight, it.currentWidth);
    if (it.indicator != nullptr) {
      it.indicator->setFrameSize(m_indicatorHeight, it.currentWidth);
    }
  } else {
    it.area->setPosition(std::round(it.currentX), 0.0f);
    it.area->setFrameSize(it.currentWidth, m_indicatorHeight);
    if (it.indicator != nullptr) {
      it.indicator->setFrameSize(it.currentWidth, m_indicatorHeight);
    }
  }
  if (it.text != nullptr) {
    it.text->setVisible(it.showLabel);
    if (it.showLabel) {
      const float itemW = m_isVertical ? m_indicatorHeight : it.currentWidth;
      const float itemH = m_isVertical ? it.currentWidth : m_indicatorHeight;
      // Box-center the (text-only) label, unrounded: the renderer snaps the glyph
      // quad to the pixel grid, so rounding here would double-round the baseline.
      const float textX = (itemW - it.text->width()) * 0.5f;
      const float textY = (itemH - it.text->height()) * 0.5f;
      it.text->setPosition(std::max(0.0f, textX), textY);
    }
  }
  if (it.indicator != nullptr) {
    const float itemW = m_isVertical ? m_indicatorHeight : it.currentWidth;
    const float itemH = m_isVertical ? it.currentWidth : m_indicatorHeight;
    it.indicator->setFrameSize(itemW, itemH);
    it.indicator->setRadius(workspacePillRadius(itemW, itemH));
  }
}

float WorkspacesWidget::workspacePillRadius(float width, float height) const noexcept {
  return resolvedBarCapsuleRadius(width, height);
}

float WorkspacesWidget::workspaceMainAxisMinWidth(float baseSize, bool active) const noexcept {
  return baseSize * (active ? m_activePillSize : m_inactivePillSize);
}

WorkspacesWidget::~WorkspacesWidget() { cancelAnimation(); }

std::optional<std::size_t> WorkspacesWidget::activeWorkspaceIndex() const {
  for (std::size_t i = 0; i < m_cachedState.size(); ++i) {
    if (m_cachedState[i].active) {
      return i;
    }
  }
  return std::nullopt;
}

void WorkspacesWidget::activateAdjacentWorkspace(int direction) {
  if (m_cachedState.empty() || direction == 0) {
    return;
  }

  const auto active = activeWorkspaceIndex();
  std::size_t targetIndex = 0;
  if (!active.has_value()) {
    targetIndex = direction > 0 ? 0 : (m_cachedState.size() - 1);
  } else {
    const std::size_t current = *active;
    if (direction > 0) {
      if (current + 1 >= m_cachedState.size()) {
        return;
      }
      targetIndex = current + 1;
    } else {
      if (current == 0) {
        return;
      }
      targetIndex = current - 1;
    }
  }

  m_platform.activateWorkspace(m_output, m_cachedState[targetIndex]);
}

std::string WorkspacesWidget::workspaceLabel(const Workspace& workspace, std::size_t displayIndex) const {
  const DisplayMode displayMode = effectiveDisplayMode();
  if (displayMode == DisplayMode::Id) {
    if (workspace.index > 0) {
      return std::to_string(workspace.index);
    }
    if (const auto numericId = numericWorkspaceId(workspace); numericId.has_value()) {
      return std::to_string(*numericId);
    }
    return std::to_string(displayIndex + 1);
  }
  if (displayMode == DisplayMode::Name) {
    std::string label = !workspace.name.empty() ? workspace.name : workspace.id;
    // Only truncate non-numeric labels (words like "VESKTOP" → "VE").
    // Numeric labels (workspace IDs like "10", "11") stay as-is.
    if (!isNumericLabel(label) && m_maxLabelChars > 0) {
      label = StringUtils::truncateUtf8CodePoints(label, m_maxLabelChars);
    }
    return label;
  }
  return {};
}

std::optional<std::size_t> WorkspacesWidget::numericWorkspaceId(const Workspace& workspace) {
  const auto parseLeadingNumber = [](const std::string& value) -> std::optional<std::size_t> {
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
      return std::nullopt;
    }

    std::size_t parsed = 0;
    std::size_t index = 0;
    while (index < value.size() && std::isdigit(static_cast<unsigned char>(value[index]))) {
      parsed = (parsed * 10) + static_cast<std::size_t>(value[index] - '0');
      ++index;
    }
    return parsed > 0 ? std::optional<std::size_t>(parsed) : std::nullopt;
  };

  if (const auto id = parseLeadingNumber(workspace.id); id.has_value()) {
    return id;
  }
  if (const auto name = parseLeadingNumber(workspace.name); name.has_value()) {
    return name;
  }
  return std::nullopt;
}

ColorSpec WorkspacesWidget::workspaceFillColor(const Workspace& workspace) const {
  if (workspace.active) {
    return m_focusedColor;
  }
  if (workspace.urgent) {
    return colorSpecFromRole(ColorRole::Error);
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = m_emptyColor;
  color.alpha *= 0.55f;
  return color;
}

ColorSpec WorkspacesWidget::workspaceTextColor(const Workspace& workspace) const {
  if (workspace.urgent) {
    return m_minimal ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnError);
  }
  if (!m_minimal) {
    return readableColorForFill(workspaceFillColor(workspace));
  }
  if (workspace.active) {
    return m_focusedColor;
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  color.alpha *= 0.55f;
  return color;
}

ColorRole WorkspacesWidget::onRoleForFill(ColorRole fill) {
  switch (fill) {
  case ColorRole::Primary:
    return ColorRole::OnPrimary;
  case ColorRole::Secondary:
    return ColorRole::OnSecondary;
  case ColorRole::Tertiary:
    return ColorRole::OnTertiary;
  case ColorRole::Error:
    return ColorRole::OnError;
  case ColorRole::Surface:
  case ColorRole::SurfaceVariant:
  case ColorRole::Outline:
  case ColorRole::Shadow:
  case ColorRole::Hover:
  case ColorRole::OnPrimary:
  case ColorRole::OnSecondary:
  case ColorRole::OnTertiary:
  case ColorRole::OnError:
  case ColorRole::OnSurface:
  case ColorRole::OnSurfaceVariant:
  case ColorRole::OnHover:
    return ColorRole::OnSurface;
  }
  return ColorRole::OnSurface;
}

ColorSpec WorkspacesWidget::readableColorForFill(const ColorSpec& fill) {
  if (fill.role.has_value()) {
    return colorSpecFromRole(onRoleForFill(*fill.role));
  }
  return fixedColorSpec(readableTextColorForBackground(resolveColorSpec(fill)));
}
