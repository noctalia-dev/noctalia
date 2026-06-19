#include "shell/bar/widgets/workspace_tray_widget.h"

#include "compositors/compositor_platform.h"
#include "core/ui_phase.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <linux/input-event-codes.h>
#include <wayland-client-protocol.h>

namespace {

  constexpr float kWidgetHeight = Style::baseGlyphSize;
  constexpr float kPaddingH = Style::spaceSm;
  constexpr float kChevronGap = Style::spaceXs;
  constexpr float kChevronSize = Style::fontSizeMini;

} // namespace

WorkspaceTrayWidget::WorkspaceTrayWidget(
    CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, std::size_t maxLabelChars,
    ColorSpec focusedColor, ColorSpec occupiedColor, bool showChevron, bool focusedOnly, bool hideWhenEmpty
)
    : m_platform(platform), m_output(output), m_displayMode(displayMode), m_maxLabelChars(maxLabelChars),
      m_focusedColor(focusedColor), m_occupiedColor(occupiedColor), m_showChevron(showChevron),
      m_focusedOnly(focusedOnly), m_hideWhenEmpty(hideWhenEmpty) {}

void WorkspaceTrayWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
  container->setOnClick([this](const InputArea::PointerData& data) {
    if (data.pressed) {
      return;
    }
    if (data.button == BTN_LEFT) {
      requestPanelToggle("workspace-tray", outputContext());
    }
  });
  container->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f) {
      return;
    }
    activateAdjacentWorkspace(delta > 0.0f ? 1 : -1);
  });
  m_container = container.get();
  setRoot(std::move(container));
}

void WorkspaceTrayWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
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

void WorkspaceTrayWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }
}

void WorkspaceTrayWidget::doUpdate(Renderer& /*renderer*/) {
  auto current = m_platform.workspaces(m_output);

  if (!m_cachedState.empty() && !current.empty() && !std::ranges::any_of(current, [](const Workspace& ws) {
        return ws.active;
      })) {
    return;
  }

  const bool showWidget = !current.empty()
      && (!m_hideWhenEmpty
          || std::ranges::any_of(current, [](const Workspace& ws) { return ws.occupied || ws.active || ws.urgent; }));
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    if (!m_cachedState.empty()) {
      m_cachedState.clear();
      m_rebuildPending = true;
      if (root() != nullptr) {
        root()->markLayoutDirty();
      }
    }
    return;
  }

  bool structuralChange = current.size() != m_cachedState.size();
  bool activeChange = false;
  bool occupiedChange = false;
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
        occupiedChange = true;
      }
    }
  }

  if (!structuralChange && !activeChange && !occupiedChange) {
    if (m_focusedOnly) {
      const bool isFocused = isFocusedOutput();
      if (isFocused != m_wasFocusedOutput) {
        m_wasFocusedOutput = isFocused;
        m_rebuildPending = true;
        if (root() != nullptr) {
          root()->markLayoutDirty();
        }
      }
    }
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

  m_rebuildPending = true;
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
}

void WorkspaceTrayWidget::rebuild(Renderer& renderer) {
  uiAssertNotRendering("WorkspaceTrayWidget::rebuild");

  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }
  m_label = nullptr;
  m_chevron = nullptr;

  const auto active = activeWorkspaceIndex();
  if (!active.has_value()) {
    m_container->setFrameSize(0.0f, 0.0f);
    return;
  }

  const std::string label = activeWorkspaceLabel();

  const float widgetHeight = std::round(kWidgetHeight * m_contentScale);
  const float labelFontSize = Style::fontSizeMini * m_contentScale;
  const float paddingH = kPaddingH * m_contentScale;
  const float chevronGlyphSize = kChevronSize * m_contentScale;
  const float chevronGap = kChevronGap * m_contentScale;
  const ColorSpec color = textColor();

  float labelWidth = 0.0f;
  float labelHeight = labelFontSize;
  if (!label.empty()) {
    const TextMetrics tm = renderer.measureText(label, labelFontSize, labelFontWeight());
    labelWidth = std::max(tm.right - tm.left, tm.inkRight - tm.inkLeft);
    labelHeight = std::max(labelHeight, tm.bottom - tm.top);
  }

  float chevronWidth = 0.0f;
  if (m_showChevron) {
    chevronWidth = chevronGlyphSize;
  }

  const float contentWidth = labelWidth + (m_showChevron ? chevronGap + chevronWidth : 0.0f);
  const float widgetWidth = contentWidth + paddingH * 2.0f;

  if (!label.empty()) {
    m_label = static_cast<Label*>(m_container->addChild(
        ui::label({
            .text = label,
            .fontSize = labelFontSize,
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = color,
            .baselineMode = LabelBaselineMode::TextFixedHeight,
        })
    ));
    m_label->measure(renderer);
  }

  if (m_showChevron) {
    m_chevron = static_cast<Glyph*>(m_container->addChild(
        ui::glyph({
            .glyph = "chevron-down",
            .glyphSize = chevronGlyphSize,
            .color = color,
        })
    ));
    m_chevron->measure(renderer);
  }

  float cursorX = paddingH;
  const float labelY = (widgetHeight - labelHeight) * 0.5f;

  if (m_label != nullptr) {
    m_label->setPosition(cursorX, labelY);
    cursorX += labelWidth;
  }

  if (m_chevron != nullptr) {
    cursorX += chevronGap;
    const float chevronY = (widgetHeight - chevronGlyphSize) * 0.5f;
    m_chevron->setPosition(cursorX, chevronY);
  }

  m_container->setFrameSize(widgetWidth, widgetHeight);

  if (barCapsuleShell() != nullptr) {
    barCapsuleShell()->markLayoutDirty();
  }
}

std::optional<std::size_t> WorkspaceTrayWidget::activeWorkspaceIndex() const {
  for (std::size_t i = 0; i < m_cachedState.size(); ++i) {
    if (m_cachedState[i].active) {
      return i;
    }
  }
  return std::nullopt;
}

void WorkspaceTrayWidget::activateAdjacentWorkspace(int direction) {
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

std::string WorkspaceTrayWidget::activeWorkspaceLabel() const {
  const auto active = activeWorkspaceIndex();
  if (!active.has_value()) {
    return {};
  }
  const auto& ws = m_cachedState[*active];

  if (m_displayMode == DisplayMode::Name) {
    std::string label = !ws.name.empty() ? ws.name : ws.id;
    if (m_maxLabelChars > 0) {
      label = StringUtils::truncateUtf8CodePoints(label, m_maxLabelChars);
    }
    return label;
  }

  if (ws.index > 0) {
    return std::to_string(ws.index);
  }
  return std::to_string(*active + 1);
}

bool WorkspaceTrayWidget::isFocusedOutput() const { return m_platform.preferredInteractiveOutput() == m_output; }

ColorSpec WorkspaceTrayWidget::textColor() const {
  if (!m_focusedOnly || isFocusedOutput()) {
    return m_focusedColor;
  }
  return m_occupiedColor;
}

std::string WorkspaceTrayWidget::outputContext() const {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(m_output));
  return buf;
}
