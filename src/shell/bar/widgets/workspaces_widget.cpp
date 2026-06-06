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

  void filterEmptyWorkspaces(std::vector<Workspace>& workspaces) {
    workspaces.erase(std::remove_if(workspaces.begin(), workspaces.end(), isEmptyWorkspace), workspaces.end());
  }

  constexpr float kWorkspaceGap = Style::spaceXs;
  constexpr float kWorkspacePillDefaultHeight = Style::barGlyphSize;
  constexpr float kWorkspaceAnimDurationMs = static_cast<float>(Style::animNormal);

  [[nodiscard]] FontWeight workspaceFontWeight(FontWeight baseWeight, bool minimal, bool active) {
    if (minimal && active) {
      return static_cast<FontWeight>(static_cast<int>(baseWeight) + 200);
    }
    return baseWeight;
  }
} // namespace

WorkspacesWidget::WorkspacesWidget(
    CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, ColorSpec focusedColor,
    ColorSpec occupiedColor, ColorSpec emptyColor, std::size_t maxLabelChars, bool labelsOnlyWhenOccupied,
    bool hideWhenEmpty, float pillScale, bool minimal
)
    : m_platform(platform), m_output(output), m_displayMode(displayMode), m_maxLabelChars(maxLabelChars),
      m_labelsOnlyWhenOccupied(labelsOnlyWhenOccupied), m_hideWhenEmpty(hideWhenEmpty), m_pillScale(pillScale),
      m_minimal(minimal), m_focusedColor(focusedColor), m_occupiedColor(occupiedColor), m_emptyColor(emptyColor) {}

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

  if (!m_cachedState.empty()
      && !current.empty()
      && !std::any_of(current.begin(), current.end(), [](const Workspace& ws) { return ws.active; })) {
    return;
  }

  if (m_hideWhenEmpty) {
    filterEmptyWorkspaces(current);
  }

  const bool showWidget = !current.empty();
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
        if (m_labelsOnlyWhenOccupied) {
          structuralChange = true;
        }
      }
      if (a.occupied != b.occupied) {
        activeChange = true;
        if (m_labelsOnlyWhenOccupied) {
          structuralChange = true;
        }
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

  // Measure text and compute per-slot widths (v4-style: proportional to char count).
  // Width = max(baseSize * factor, textWidth + padding)
  //   factor: 2.2 for active, 1.0 for inactive
  //   padding: baseSize * 0.6
  struct SlotMetrics {
    std::string label;
    bool showLabel = false;
    bool isNumeric = false;
    float textWidth = 0.0f;
    float inkCenterOffset = 0.0f;
    float inkVCenterOffset = 0.0f;
    float inactiveWidth = 0.0f;
    float activeWidth = 0.0f;
  };
  std::vector<SlotMetrics> slots(workspaces.size());

  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
    slot.label = labels[i];
    slot.showLabel = shouldShowWorkspaceLabel(workspaces[i], labels[i]);

    // Detect numeric labels (workspace IDs like "1", "10", "11")
    slot.isNumeric = !labels[i].empty() && std::all_of(labels[i].begin(), labels[i].end(), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c));
    });

    if (slot.showLabel) {
      const FontWeight slotFontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, workspaces[i].active);
      const TextMetrics tm = renderer.measureText(labels[i], labelFontSize, slotFontWeight);
      slot.textWidth = std::max(tm.right - tm.left, tm.inkRight - tm.inkLeft);
      const float logicalCenter = (tm.left + tm.right) * 0.5f;
      const float inkCenter = (tm.inkLeft + tm.inkRight) * 0.5f;
      slot.inkCenterOffset = slot.isNumeric ? 0.0f : (inkCenter - logicalCenter);
      const float logicalVCenter = (tm.top + tm.bottom) * 0.5f;
      const float inkVCenter = (tm.inkTop + tm.inkBottom) * 0.5f;
      slot.inkVCenterOffset = inkVCenter - logicalVCenter;
    }
  }

  const float baseSize = std::round(pillHeight);
  const float padding = m_minimal ? (Style::spaceXs * m_contentScale) : (baseSize * 0.6f);
  constexpr float kActiveFactor = 2.2f;
  constexpr float kInactiveFactor = 1.0f;

  float maxLabelHeight = labelFontSize;
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    auto& slot = slots[i];
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

    const float minWidth = baseSize * kInactiveFactor;
    const float minActiveWidth = baseSize * kActiveFactor;

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
    item.inkCenterOffset = slot.inkCenterOffset;
    item.inkVCenterOffset = slot.inkVCenterOffset;

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
              .color = workspaceTextColor(ws),
              .fontWeight = workspaceFontWeight(configuredFontWeight, m_minimal, ws.active),
              .baselineMode = m_isVertical ? std::optional<LabelBaselineMode>{LabelBaselineMode::InkCentered}
                                           : std::optional<LabelBaselineMode>{},
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

  // Size the container now that per-item widths are known.
  float total = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    const float itemWidth = (m_cachedState[i].active) ? m_items[i].activeWidth : m_items[i].inactiveWidth;
    total += itemWidth;
  }
  if (m_items.size() > 1) {
    total += gap * static_cast<float>(m_items.size() - 1);
  }
  if (m_isVertical) {
    m_container->setFrameSize(m_indicatorHeight, total);
  } else {
    m_container->setFrameSize(total, m_indicatorHeight);
  }

  // Snap to targets immediately (no animation on structural rebuild).
  computeTargets();
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    it.currentX = it.targetX;
    it.currentWidth = it.targetWidth;
    applyItemLayout(i);
  }
}

void WorkspacesWidget::computeTargets() {
  float cursor = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const float w = (m_cachedState[i].active) ? it.activeWidth : it.inactiveWidth;
    it.targetX = cursor;
    it.targetWidth = w;
    it.active = m_cachedState[i].active;
    cursor += w + m_gap;
  }
}

void WorkspacesWidget::updateContainerSize() {
  if (m_container == nullptr || m_items.empty()) {
    return;
  }
  float total = 0.0f;
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    total += m_items[i].currentWidth;
  }
  if (m_items.size() > 1) {
    total += m_gap * static_cast<float>(m_items.size() - 1);
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

void WorkspacesWidget::retarget(Renderer& renderer) {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    auto& it = m_items[i];
    const auto& ws = m_cachedState[i];
    const std::string label = workspaceLabel(ws, i);
    const FontWeight fontWeight = workspaceFontWeight(labelFontWeight(), m_minimal, ws.active);
    const bool labelChanged = it.label != label;
    if (labelChanged) {
      it.label = label;
      if (it.text != nullptr) {
        it.text->setText(label);
      }
    }
    if (it.text != nullptr) {
      const bool weightChanged = it.text->fontWeight() != fontWeight;
      it.text->setFontWeight(fontWeight);
      if (labelChanged || weightChanged) {
        it.text->measure(renderer);
        const float fontSize = it.text->fontSize();
        const TextMetrics tm = renderer.measureText(label, fontSize, fontWeight);
        const float logCenter = (tm.left + tm.right) * 0.5f;
        const float inkCenter = (tm.inkLeft + tm.inkRight) * 0.5f;
        it.inkCenterOffset = inkCenter - logCenter;
        const float logVCenter = (tm.top + tm.bottom) * 0.5f;
        const float inkVCenter = (tm.inkTop + tm.inkBottom) * 0.5f;
        it.inkVCenterOffset = inkVCenter - logVCenter;
      }
    }
    if (it.indicator != nullptr) {
      it.indicator->setFill(workspaceFillColor(ws));
      it.indicator->clearBorder();
    }
    if (it.text != nullptr) {
      it.text->setColor(workspaceTextColor(ws));
    }
  }

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
      const float textX = std::round((itemW - it.text->width()) * 0.5f - it.inkCenterOffset);
      const float textY = std::round((itemH - it.text->height()) * 0.5f - it.inkVCenterOffset);
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
    const bool isNumeric = !label.empty()
        && std::all_of(label.begin(), label.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
    if (!isNumeric && m_maxLabelChars > 0) {
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
