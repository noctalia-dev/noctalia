#include "shell/bar/widgets/taskbar_widget.h"

#include "compositors/compositor_detect.h"
#include "compositors/workspace_backend.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_seat.h"
#include "wayland/wayland_toplevels.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <wayland-client-protocol.h>

namespace {

  // Integer centering; optional odd spare pixel on the end side (right/bottom).
  [[nodiscard]] float centeredOffset(float extent, float content, float inset = 0.0f, bool oddSpareOnEnd = true) {
    const float inner = std::max(0.0f, extent - inset * 2.0f);
    const int innerPx = static_cast<int>(std::lround(inner));
    const int contentPx = static_cast<int>(std::lround(content));
    const int spare = std::max(0, innerPx - contentPx);
    const int start = oddSpareOnEnd ? (spare / 2) : (spare / 2 + (spare % 2));
    return inset + static_cast<float>(start);
  }

  struct ExternalBadgePosition {
    float left = 0.0f;
    float top = 0.0f;
  };

  [[nodiscard]] float externalBadgeMainStartInset(WorkspaceLabelPlacement placement, float badgeMain) {
    if (placement == WorkspaceLabelPlacement::Corner) {
      return badgeMain * 0.2f;
    }
    if (placement == WorkspaceLabelPlacement::Centered) {
      return badgeMain * 0.4f;
    }
    return 0.0f;
  }

  [[nodiscard]] ExternalBadgePosition externalBadgePosition(
      WorkspaceLabelPlacement placement, bool vertical, float groupWidth, float groupHeight, float badgeWidth,
      float badgeHeight, float outlineInset
  ) {
    if (placement == WorkspaceLabelPlacement::Corner) {
      if (vertical) {
        return {centeredOffset(groupWidth, badgeWidth, outlineInset, false), std::round(outlineInset)};
      }
      return {std::round(badgeWidth * -0.2f), 0.0f};
    }
    if (vertical) {
      return {centeredOffset(groupWidth, badgeWidth, outlineInset, false), std::round(outlineInset)};
    }
    return {std::round(-badgeWidth * 0.4f), centeredOffset(groupHeight, badgeHeight, outlineInset, false)};
  }

  [[nodiscard]] float fitBadgeFontSize(
      Renderer& renderer, std::string_view label, float maxWidth, float maxHeight, float scale, FontWeight fontWeight
  ) {
    float fontSize = std::round(Style::fontSizeMini * scale);
    const float minFontSize = std::round(8.0f * scale);
    const float maxTextWidth = maxWidth * 0.82f;
    const float maxTextHeight = maxHeight * 0.82f;
    while (fontSize >= minFontSize) {
      const auto metrics = renderer.measureText(label, fontSize, fontWeight);
      const float textWidth = std::max(0.0f, metrics.right - metrics.left);
      const float textHeight = std::max(0.0f, metrics.bottom - metrics.top);
      if (textWidth <= maxTextWidth && textHeight <= maxTextHeight) {
        return fontSize;
      }
      fontSize -= 1.0f;
    }
    return minFontSize;
  }

  struct WorkspaceDiscSize {
    float width = 0.0f;
    float height = 0.0f;
  };

  [[nodiscard]] WorkspaceDiscSize measureWorkspaceDiscSize(
      Renderer& renderer, std::string_view label, float fontSize, float minHeight, float scale, FontWeight fontWeight
  ) {
    const auto metrics = renderer.measureText(label, fontSize, fontWeight);
    const float textW = std::max(0.0f, metrics.right - metrics.left);
    const float pad = Style::spaceXs * scale;
    WorkspaceDiscSize size{};
    size.height = minHeight;
    size.width = std::round(std::max(minHeight, textW + pad * 2.0f));
    return size;
  }

  [[nodiscard]] WorkspaceDiscSize clampWorkspaceDiscToCrossLimit(WorkspaceDiscSize disc, float maxCross) {
    if (maxCross <= 0.0f) {
      return disc;
    }
    const float limit = std::floor(maxCross);
    disc.width = std::min(disc.width, limit);
    disc.height = std::min(disc.height, limit);
    return disc;
  }

  [[nodiscard]] std::uintptr_t taskHandleKey(const ToplevelInfo& window) {
    if (window.handle != nullptr) {
      return reinterpret_cast<std::uintptr_t>(window.handle);
    }
    if (window.extHandle != nullptr) {
      return reinterpret_cast<std::uintptr_t>(window.extHandle);
    }
    if (!window.identifier.empty()) {
      const std::uintptr_t key = static_cast<std::uintptr_t>(std::hash<std::string>{}(window.identifier));
      return key == 0 ? 1 : key;
    }
    if (!window.appId.empty() || !window.title.empty()) {
      const std::uintptr_t key =
          static_cast<std::uintptr_t>(std::hash<std::string>{}(window.appId + "\n" + window.title));
      return key == 0 ? 1 : key;
    }
    return 0;
  }

  [[nodiscard]] float barCapsuleThicknessFor(const ConfigService& config, std::string_view barName) {
    for (const auto& bar : config.config().bars) {
      if (bar.name == barName) {
        return bar.capsuleThickness;
      }
    }
    return 0.76f;
  }

  [[nodiscard]] float taskbarShellCross(float barCross, bool barCapsuleEnabled, float capsuleThickness) {
    if (!barCapsuleEnabled || barCross <= 0.0f) {
      return barCross;
    }
    return std::max(1.0f, std::round(barCross * capsuleThickness));
  }

  // Inner cross budget for workspace group capsules nested inside a bar widget capsule.
  [[nodiscard]] float taskbarGroupedCrossBudget(
      float shellCross, bool barCapsuleEnabled, bool workspaceGroupCapsule, bool barCapsuleBorder, float scale
  ) {
    if (!barCapsuleEnabled || !workspaceGroupCapsule || shellCross <= 0.0f) {
      return shellCross;
    }
    const float outerBorder = barCapsuleBorder ? Style::borderWidth * scale : 0.0f;
    const float innerBorder = Style::borderWidth * scale;
    const float nestGap = std::round(std::max(1.0f, Style::spaceXs * 0.25f * scale));
    return std::max(0.0f, shellCross - 2.0f * (outerBorder + innerBorder + nestGap));
  }

} // namespace

TaskbarWidget::TaskbarWidget(
    CompositorPlatform& platform, ConfigService& config, wl_output* output, TaskbarWidgetOptions options
)
    : m_platform(platform), m_configService(config), m_output(output), m_configOptions(std::move(options)),
      m_showAllOutputs(m_configOptions.showAllOutputs), m_focusedOutputOnly(m_configOptions.focusedOutputOnly),
      m_minimal(m_configOptions.minimal), m_enableScroll(m_configOptions.enableScroll),
      m_showActiveIndicator(m_configOptions.showActiveIndicator), m_activeOpacity(m_configOptions.activeOpacity),
      m_inactiveOpacity(m_configOptions.inactiveOpacity), m_focusedColor(m_configOptions.focusedColor),
      m_occupiedColor(m_configOptions.occupiedColor), m_emptyColor(m_configOptions.emptyColor),
      m_urgentColor(m_configOptions.urgentColor), m_windowTitleMaxWidth(m_configOptions.windowTitleMaxWidth),
      m_taskbarMaxWidth(m_configOptions.taskbarMaxWidth), m_barPosition(std::move(m_configOptions.barPosition)),
      m_barName(std::move(m_configOptions.barName)) {
  syncWorkspaceGroupingCapability();
  buildDesktopIconIndex();
}

void TaskbarWidget::syncWorkspaceGroupingCapability() {
  const bool supported = m_platform.supportsTaskbarWorkspaceGrouping();
  const bool groupByWorkspace = supported && m_configOptions.groupByWorkspace;
  const bool onlyActiveWorkspace = supported && m_configOptions.onlyActiveWorkspace;
  const bool showWorkspaceLabel = !supported || m_configOptions.showWorkspaceLabel;
  const bool hideEmptyWorkspaces = supported && m_configOptions.hideEmptyWorkspaces;
  const bool workspaceGroupCapsule = !supported || m_configOptions.workspaceGroupCapsule;
  const bool groupSingleIconPerApp = supported && m_configOptions.groupSingleIconPerApp;
  const bool showWindowTitle =
      m_configOptions.showWindowTitle && m_barPosition != "left" && m_barPosition != "right" && !groupByWorkspace;

  const bool changed = groupByWorkspace != m_groupByWorkspace
      || onlyActiveWorkspace != m_onlyActiveWorkspace
      || showWorkspaceLabel != m_showWorkspaceLabel
      || m_workspaceLabelPlacement != m_configOptions.workspaceLabelPlacement
      || hideEmptyWorkspaces != m_hideEmptyWorkspaces
      || workspaceGroupCapsule != m_workspaceGroupCapsule
      || groupSingleIconPerApp != m_groupSingleIconPerApp
      || showWindowTitle != m_showWindowTitle;

  m_groupByWorkspace = groupByWorkspace;
  m_onlyActiveWorkspace = onlyActiveWorkspace;
  m_showWorkspaceLabel = showWorkspaceLabel;
  m_workspaceLabelPlacement = supported ? m_configOptions.workspaceLabelPlacement : WorkspaceLabelPlacement::Corner;
  m_hideEmptyWorkspaces = hideEmptyWorkspaces;
  m_workspaceGroupCapsule = workspaceGroupCapsule;
  m_groupSingleIconPerApp = groupSingleIconPerApp;
  m_showWindowTitle = showWindowTitle;

  if (changed) {
    m_rebuildPending = true;
    if (root() != nullptr) {
      root()->markLayoutDirty();
    }
  }
}

TaskbarWidget::~TaskbarWidget() = default;

bool TaskbarWidget::taskInWorkspaceGroup(const TaskModel& task, const WorkspaceModel& ws) {
  if (task.workspaceKey.empty()) {
    return false;
  }
  if (task.workspaceKey == ws.key) {
    return true;
  }
  if (!ws.workspace.id.empty() && task.workspaceKey == ws.workspace.id) {
    return true;
  }
  return !ws.workspace.name.empty() && task.workspaceKey == ws.workspace.name;
}

void TaskbarWidget::activateTaskModel(const TaskModel& task) {
  if (task.firstHandle != nullptr) {
    m_platform.activateToplevel(task.firstHandle);
    return;
  }
  if (compositors::isKde() && (!task.title.empty() || !task.appId.empty())) {
    m_platform.activateKdeWindow(task.title, task.appId, task.workspaceWindowId);
    return;
  }
  if (!task.workspaceWindowId.empty()) {
    m_platform.focusCompositorWindow(task.workspaceWindowId);
    return;
  }
  if (!task.workspaceKey.empty()) {
    for (const auto& workspace : m_workspaces) {
      if (taskInWorkspaceGroup(task, workspace)) {
        m_platform.activateWorkspace(workspaceHostOutput(workspace), workspace.workspace);
        return;
      }
    }
  }
}

void TaskbarWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_enableScroll) {
      return false;
    }
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && data.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return false;
    }

    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return false;
    }
    if (m_groupByWorkspace) {
      activateAdjacentWorkspace(steps > 0.0f ? 1 : -1);
    } else {
      activateAdjacentTask(steps > 0.0f ? 1 : -1);
    }
    return true;
  });

  auto root = ui::row({
      .out = &m_root,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm,
  });

  root->addChild(
      ui::row({
          .out = &m_taskStrip,
          .align = FlexAlign::Center,
          .gap = Style::spaceSm,
      })
  );

  container->addChild(std::move(root));
  setRoot(std::move(container));

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() {
    m_rebuildPending = true;
    requestUpdate();
  });
}

void TaskbarWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_root == nullptr || m_taskStrip == nullptr) {
    return;
  }

  const bool wasVertical = m_vertical;
  m_vertical = containerHeight > containerWidth;
  if (m_vertical != wasVertical) {
    m_rebuildPending = true;
  }
  m_containerWidth = containerWidth;
  m_containerHeight = containerHeight;
  const std::uint64_t textMetricsGeneration = renderer.textMetricsGeneration();
  if (m_textMetricsGeneration != textMetricsGeneration) {
    m_textMetricsGeneration = textMetricsGeneration;
    m_rebuildPending = true;
  }

  m_root->setDirection(m_vertical ? FlexDirection::Vertical : FlexDirection::Horizontal);
  m_root->setAlign(FlexAlign::Center);
  m_root->setGap(Style::spaceSm * m_contentScale);

  m_taskStrip->setDirection(m_vertical ? FlexDirection::Vertical : FlexDirection::Horizontal);
  m_taskStrip->setAlign(FlexAlign::Center);
  if (!m_groupByWorkspace) {
    m_taskStrip->setGap(Style::spaceSm * m_contentScale);
  }

  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  }

  m_root->layout(renderer);
  if (Node* container = root(); container != nullptr && container != m_root) {
    container->setFrameSize(m_root->width(), m_root->height());
  }
}

void TaskbarWidget::doUpdate(Renderer& /*renderer*/) {
  updateModels();
  if (m_focusedOutputOnly) {
    const bool isFocused = isFocusedOutput();
    if (isFocused != m_wasFocusedOutput) {
      m_wasFocusedOutput = isFocused;
      m_rebuildPending = true;
      if (root() != nullptr) {
        root()->markLayoutDirty();
      }
    }
  }
}

void TaskbarWidget::rebuild(Renderer& renderer) {
  if (m_taskStrip == nullptr) {
    return;
  }
  m_activeUsesFocusedColor = !m_focusedOutputOnly || isFocusedOutput();
  clearChildren(m_taskStrip);
  buildTaskButtons(renderer);
}

void TaskbarWidget::clearChildren(Flex* flex) const {
  while (flex != nullptr && !flex->children().empty()) {
    flex->removeChild(flex->children().back().get());
  }
}

void TaskbarWidget::buildTaskButtons(Renderer& renderer) {
  if (m_taskStrip == nullptr) {
    return;
  }
  float iconSize = std::round(Style::baseGlyphSize * m_contentScale);
  float tilePadding = Style::spaceXs * 0.35f * m_contentScale;
  float tileSize = std::round(iconSize + tilePadding * 2.0f);
  const float barCross = m_vertical ? m_containerWidth : m_containerHeight;
  const float capsuleThickness = barCapsuleThicknessFor(m_configService, m_barName);
  const bool barCapsule = barCapsuleSpec().enabled;
  const float shellCross = taskbarShellCross(barCross, barCapsule, capsuleThickness);
  const float crossExtent = taskbarGroupedCrossBudget(
      shellCross, barCapsule, m_groupByWorkspace && m_workspaceGroupCapsule, barCapsuleSpec().border.has_value(),
      m_contentScale
  );
  const float groupBorderInset = Style::borderWidth * m_contentScale;
  const float groupOutlineInset = m_workspaceGroupCapsule ? groupBorderInset : 0.0f;
  const float groupedCrossInner = crossExtent > 0.0f && m_groupByWorkspace && m_workspaceGroupCapsule
      ? std::max(0.0f, crossExtent - groupBorderInset * 2.0f)
      : crossExtent;
  const float groupedMinPad = std::round(std::max(1.0f, Style::spaceXs * 0.35f * m_contentScale));
  const float tileCrossLimit = m_groupByWorkspace && m_workspaceGroupCapsule
      ? std::max(0.0f, groupedCrossInner - groupedMinPad * 2.0f)
      : crossExtent;
  if (crossExtent > 0.0f && tileSize > tileCrossLimit + 0.5f) {
    const float maxTile = std::max(0.0f, tileCrossLimit);
    const float padCap = std::floor(std::max(0.0f, (maxTile - iconSize) * 0.5f));
    tilePadding = std::min(tilePadding, padCap);
    tileSize = std::round(iconSize + tilePadding * 2.0f);
    if (tileSize > maxTile + 0.5f) {
      iconSize = std::floor(std::max(0.0f, maxTile - tilePadding * 2.0f));
      tileSize = std::round(iconSize + tilePadding * 2.0f);
    }
  }
  const float tileGap = Style::spaceSm * m_contentScale;

  const FontWeight fontWeight = labelFontWeight();
  const std::string fontFamily = labelFontFamily();

  const float maxTileWidth = m_tasks.empty()
      ? m_taskbarMaxWidth * m_contentScale
      : std::floor(
            (m_taskbarMaxWidth * m_contentScale - tileGap * static_cast<float>(m_tasks.size() - 1))
            / static_cast<float>(m_tasks.size())
        );
  // If the title text is too narrow, all it shows is "..." which isn't useful, so we hide it instead.
  const auto metric = renderer.measureText("...", Style::fontSizeCaption * m_contentScale, fontWeight);
  const float minWindowTitleWidth = (metric.right - metric.left) * 2;
  const float windowTitleGap = Style::spaceXs * m_contentScale;
  const float windowTitleWidth =
      std::min(m_windowTitleMaxWidth * m_contentScale, std::max(0.0f, maxTileWidth - tileSize - windowTitleGap));
  const bool showWindowTitle = m_showWindowTitle && windowTitleWidth > minWindowTitleWidth;
  const float tileWidthWithTitle = tileSize + (showWindowTitle ? windowTitleWidth + windowTitleGap : 0.0f);

  const auto workspaceAxisHandler = [this](const InputArea::PointerData& data) -> bool {
    if (!m_enableScroll || !m_groupByWorkspace) {
      return false;
    }
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && data.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return false;
    }

    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return false;
    }

    activateAdjacentWorkspace(steps > 0.0f ? 1 : -1);
    return true;
  };

  auto attachHover = [this](InputArea& area, float width, float height) {
    auto hoverBox = ui::box({
        .radius = resolvedBarCapsuleRadius(width, height),
        .width = width,
        .height = height,
        .configure = [](Box& box) { box.setZIndex(-1); },
    });
    hoverBox->setHitTestVisible(false);
    hoverBox->setVisible(false);
    auto* hoverBoxPtr = static_cast<Box*>(area.addChild(std::move(hoverBox)));

    auto progress = std::make_shared<float>(0.0f);
    area.setOnEnter([this, hoverBoxPtr, progress](const InputArea::PointerData&) {
      if (m_animations == nullptr)
        return;
      m_animations->cancelForOwner(hoverBoxPtr);
      const ColorSpec fill = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
      m_animations->animate(
          *progress, 1.0f, Style::animFast, Easing::EaseOutCubic,
          [this, hoverBoxPtr, fill, progress](float p) {
            *progress = p;
            hoverBoxPtr->setVisible(p > 0.001f);
            ColorSpec c = fill;
            c.alpha = 0.1f * p;
            hoverBoxPtr->setFill(c);
            requestRedraw();
          },
          {}, hoverBoxPtr
      );
      requestFrameTick();
    });
    area.setOnLeave([this, hoverBoxPtr, progress]() {
      if (m_animations == nullptr)
        return;
      m_animations->cancelForOwner(hoverBoxPtr);
      const ColorSpec fill = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
      m_animations->animate(
          *progress, 0.0f, Style::animFast, Easing::EaseOutCubic,
          [this, hoverBoxPtr, fill, progress](float p) {
            *progress = p;
            hoverBoxPtr->setVisible(p > 0.001f);
            ColorSpec c = fill;
            c.alpha = 0.1f * p;
            hoverBoxPtr->setFill(c);
            requestRedraw();
          },
          {}, hoverBoxPtr
      );
      requestFrameTick();
    });
  };

  auto createTaskTile = [&](const TaskModel& task, std::vector<TaskModel> cycleCandidates = {},
                            std::string cycleKey = {}, std::size_t badgeCount = 1) {
    auto area = std::make_unique<InputArea>();
    area->setFrameSize(tileWidthWithTitle, tileSize);
    area->setOpacity(task.active ? m_activeOpacity : m_inactiveOpacity);
    area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
    area->setOnAxisHandler(workspaceAxisHandler);

    const WorkspaceModel* taskWorkspace = nullptr;
    if (m_groupByWorkspace && !task.workspaceKey.empty()) {
      for (const auto& workspace : m_workspaces) {
        if (taskInWorkspaceGroup(task, workspace)) {
          taskWorkspace = &workspace;
          break;
        }
      }
    }
    const std::optional<Workspace> clickWorkspace =
        taskWorkspace != nullptr ? std::optional<Workspace>(taskWorkspace->workspace) : std::nullopt;
    wl_output* const taskWsHost = taskWorkspace != nullptr ? workspaceHostOutput(*taskWorkspace) : m_output;

    if (task.firstHandle != nullptr
        || !task.workspaceWindowId.empty()
        || (compositors::isKde() && (!task.title.empty() || !task.appId.empty()))
        || clickWorkspace.has_value()
        || !cycleCandidates.empty()) {
      auto* areaPtr = area.get();
      area->setOnClick([this, task, areaPtr, handle = task.firstHandle, windowId = task.workspaceWindowId,
                        clickWorkspace, taskWsHost, cycleCandidates = std::move(cycleCandidates),
                        cycleKey = std::move(cycleKey)](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          if (!cycleCandidates.empty()) {
            std::size_t& cursor = m_groupedAppCycleCursor[cycleKey];
            if (cursor >= cycleCandidates.size()) {
              cursor = 0;
            }
            const TaskModel& target = cycleCandidates[cursor];
            cursor = (cursor + 1) % cycleCandidates.size();
            activateTaskModel(target);
            return;
          }
          if (handle != nullptr) {
            m_platform.activateToplevel(handle);
            return;
          }
          if (!windowId.empty()) {
            m_platform.focusCompositorWindow(windowId);
            return;
          }
          if (compositors::isKde() && (!task.title.empty() || !task.appId.empty())) {
            m_platform.activateKdeWindow(task.title, task.appId);
            return;
          }
          if (clickWorkspace.has_value()) {
            m_platform.activateWorkspace(taskWsHost, *clickWorkspace);
          }
          return;
        }
        if (data.button == BTN_RIGHT && areaPtr != nullptr && handle != nullptr) {
          openTaskContextMenu(task, *areaPtr);
        }
      });
    } else {
      area->setEnabled(false);
    }

    auto content = ui::flex(
        m_vertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
        {
            .align = FlexAlign::Center,
            .justify = showWindowTitle ? FlexJustify::Start : FlexJustify::Center,
            .gap = showWindowTitle ? windowTitleGap : tilePadding,
            .width = tileWidthWithTitle,
            .height = tileSize,
        }
    );

    if (!task.iconPath.empty()) {
      auto image = ui::image({
          .fit = ImageFit::Contain,
          .width = iconSize,
          .height = iconSize,
      });
      image->setAppIconColorization(effectiveShellAppIconColorizationTint(m_configService.config().shell));
      image->setSourceFile(renderer, task.iconPath, static_cast<int>(std::round(iconSize)), true);
      if (image->hasImage()) {
        content->addChild(std::move(image));
      } else {
        auto glyph = ui::glyph({
            .glyph = "app-window",
            .glyphSize = iconSize,
        });
        glyph->measure(renderer);
        content->addChild(std::move(glyph));
      }
    } else {
      auto glyph = ui::glyph({
          .glyph = "apps",
          .glyphSize = iconSize,
      });
      glyph->measure(renderer);
      content->addChild(std::move(glyph));
    }

    if (showWindowTitle) {
      auto label = ui::label({
          .text = task.title,
          .fontSize = Style::fontSizeCaption * m_contentScale,
          .fontWeight = fontWeight,
          .fontFamily = fontFamily,
          .maxWidth = windowTitleWidth,
          .maxLines = 1,
      });
      label->measure(renderer);
      content->addChild(std::move(label));
    }

    area->addChild(std::move(content));

    if (badgeCount > 1) {
      const std::size_t dotCount = badgeCount >= 4 ? 3U : (badgeCount == 3 ? 2U : 1U);
      const float dotSize = std::round(std::max(2.0f, Style::baseGlyphSize * 0.16f * m_contentScale));
      const float dotGap = std::round(std::max(1.0f, dotSize * 0.55f));
      const float runHeight =
          dotSize * static_cast<float>(dotCount) + dotGap * static_cast<float>(dotCount > 0 ? dotCount - 1 : 0);
      const float iconRightInset = std::round(std::max(1.0f, iconSize * 0.08f));
      const float dotX = std::round(centeredOffset(tileSize, iconSize) + iconSize - dotSize - iconRightInset);
      const float startY = std::round(centeredOffset(tileSize, iconSize) + (iconSize - runHeight) * 0.5f);
      const ColorSpec dotColor = colorSpecFromRole(ColorRole::Primary, 0.9f);

      for (std::size_t i = 0; i < dotCount; ++i) {
        auto dot = ui::box({
            .fill = dotColor,
            .radius = resolvedBarCapsuleRadius(dotSize, dotSize),
            .width = dotSize,
            .height = dotSize,
        });
        dot->setPosition(dotX, std::round(startY + static_cast<float>(i) * (dotSize + dotGap)));
        area->addChild(std::move(dot));
      }
    }

    if (task.active && m_showActiveIndicator) {
      const float d = std::max(4.0f, std::round(Style::baseGlyphSize * 0.32f * m_contentScale));
      const float groupedCapsuleInset =
          (m_groupByWorkspace && m_workspaceGroupCapsule ? Style::borderWidth : 0.0f) * m_contentScale;
      const float bottomInset = 0.25f * m_contentScale + groupedCapsuleInset;
      if (showWindowTitle) {
        const float lineThickness = d * 0.5f;
        auto indicator = ui::box({
            .fill = colorSpecFromRole(ColorRole::Primary),
            .radius = lineThickness * 0.5f,
            .width = tileWidthWithTitle - tilePadding * 2,
            .height = lineThickness,
        });
        indicator->setPosition(tilePadding, std::round(tileSize));
        area->addChild(std::move(indicator));
      } else {
        auto indicator = ui::box({
            .fill = colorSpecFromRole(ColorRole::Primary),
            .radius = resolvedBarCapsuleRadius(d, d),
            .width = d,
            .height = d,
        });
        indicator->setPosition(std::round((tileSize - d) * 0.5f), std::round(tileSize - d - bottomInset));
        area->addChild(std::move(indicator));
      }
    }
    if (!task.title.empty()) {
      area->setTooltip(task.title);
    }
    attachHover(*area, tileWidthWithTitle, tileSize);
    return area;
  };

  if (m_groupByWorkspace && !m_workspaces.empty()) {
    const float groupGap = Style::spaceXs * m_contentScale;
    const float groupPad = Style::spaceXs * m_contentScale;

    const bool inlineBadge = m_showWorkspaceLabel && m_workspaceLabelPlacement == WorkspaceLabelPlacement::Inside;
    const bool externalBadge = m_showWorkspaceLabel && !inlineBadge;
    const float badgeBase = std::round(std::max(11.0f, Style::baseGlyphSize * 0.72f) * m_contentScale);
    const float externalBadgeFontSize = std::round(Style::fontSizeCaption * 0.72f * m_contentScale);

    const float externalBadgeCrossLimit = m_vertical && crossExtent > 0.0f
        ? std::max(0.0f, (m_workspaceGroupCapsule ? groupedCrossInner : crossExtent) - 2.0f * groupOutlineInset)
        : 0.0f;

    float stripGap = groupGap;
    float stripPaddingMainStart = 0.0f;
    float stripPaddingCrossStart = 0.0f;
    if (externalBadge) {
      const float badgeGap = std::round(std::max(1.0f, Style::spaceXs * 0.5f * m_contentScale));
      float maxMainStart = 0.0f;
      for (const auto& wsm : m_workspaces) {
        auto measuredDisc =
            measureWorkspaceDiscSize(renderer, wsm.label, externalBadgeFontSize, badgeBase, m_contentScale, fontWeight);
        if (m_vertical) {
          measuredDisc = clampWorkspaceDiscToCrossLimit(measuredDisc, externalBadgeCrossLimit);
        }
        const float badgeMain = m_vertical ? measuredDisc.height : measuredDisc.width;
        maxMainStart = std::max(maxMainStart, externalBadgeMainStartInset(m_workspaceLabelPlacement, badgeMain));
      }
      stripPaddingMainStart = std::round(maxMainStart);
      if (m_vertical) {
        stripPaddingMainStart = 0.0f;
        stripPaddingCrossStart = 0.0f;
        stripGap = groupGap;
      } else {
        stripGap = std::max(stripGap, stripPaddingMainStart + badgeGap);
      }
    }
    m_taskStrip->setGap(stripGap);
    if (m_vertical) {
      m_taskStrip->setPadding(stripPaddingMainStart, 0.0f, 0.0f, stripPaddingCrossStart);
    } else {
      m_taskStrip->setPadding(stripPaddingCrossStart, 0.0f, 0.0f, stripPaddingMainStart);
    }

    auto createWorkspaceBadge = [&](const WorkspaceModel& ws, const WorkspaceDiscSize& disc, bool hover) {
      Button::ButtonPalette badgePalette{};
      const ColorSpec fill = m_minimal ? clearColorSpec() : workspaceFillColor(ws.workspace);
      const ColorSpec text = workspaceTextColor(ws.workspace);
      badgePalette.normal = Button::ButtonStateColors{fill, clearColorSpec(), text};
      badgePalette.hover = badgePalette.normal;
      badgePalette.pressed = badgePalette.normal;
      badgePalette.disabled = badgePalette.normal;
      badgePalette.borderWidth = 0.0f;

      const float badgeFontSize =
          fitBadgeFontSize(renderer, ws.label, disc.width, disc.height, m_contentScale, fontWeight);

      auto badge = ui::button({
          .text = ws.label,
          .fontSize = badgeFontSize,
          .customPalette = badgePalette,
          .minWidth = disc.width,
          .minHeight = disc.height,
          .maxWidth = disc.width,
          .maxHeight = disc.height,
          .padding = 0.0f,
          .radius = resolvedBarCapsuleRadius(disc.width, disc.height),
          .width = disc.width,
          .height = disc.height,
          .configure = [this, fontWeight, fontFamily](Button& b) {
            if (b.label() != nullptr) {
              b.label()->setFontWeight(fontWeight);
              b.label()->setFontFamily(fontFamily);
              b.label()->setTextAlign(TextAlign::Center);
            }
          },
      });

      auto wsCopy = ws.workspace;
      wl_output* const wsHost = workspaceHostOutput(ws);
      badge->setOnClick([this, wsCopy, wsHost]() { m_platform.activateWorkspace(wsHost, wsCopy); });
      badge->inputArea()->setOnAxisHandler(workspaceAxisHandler);

      if (hover) {
        attachHover(*badge->inputArea(), disc.width, disc.height);
      }
      return badge;
    };

    std::unordered_set<std::string> cycleKeysThisFrame;
    for (const auto& ws : m_workspaces) {
      std::vector<const TaskModel*> tasks;
      for (const auto& task : m_tasks) {
        if (taskInWorkspaceGroup(task, ws)) {
          tasks.push_back(&task);
        }
      }
      std::ranges::stable_sort(tasks, [](const TaskModel* lhs, const TaskModel* rhs) {
        if (lhs->workspaceOrder != rhs->workspaceOrder) {
          return lhs->workspaceOrder < rhs->workspaceOrder;
        }
        if (lhs->order != rhs->order) {
          return lhs->order < rhs->order;
        }
        return lhs->handleKey < rhs->handleKey;
      });

      std::vector<const TaskModel*> renderedTasks = tasks;
      std::unordered_map<std::uintptr_t, std::vector<TaskModel>> cycleCandidatesByHandle;
      std::unordered_map<std::uintptr_t, std::string> cycleKeyByHandle;
      std::unordered_map<std::uintptr_t, std::size_t> badgeCountByHandle;
      if (m_groupSingleIconPerApp && !tasks.empty()) {
        struct GroupedTaskItem {
          const TaskModel* representative = nullptr;
          std::string cycleKey;
          std::vector<TaskModel> candidates;
        };
        std::vector<GroupedTaskItem> groupedItems;
        std::unordered_map<std::string, std::size_t> groupedIndexByKey;
        groupedItems.reserve(tasks.size());
        groupedIndexByKey.reserve(tasks.size());

        const std::string cyclePrefix = ws.key + '\n';
        for (const TaskModel* task : tasks) {
          std::string appKey =
              !task->appIdLower.empty() ? task->appIdLower : (!task->idLower.empty() ? task->idLower : task->nameLower);
          if (appKey.empty()) {
            appKey = std::to_string(task->handleKey);
          }
          const std::string groupedKey = cyclePrefix + appKey;
          const auto [it, inserted] = groupedIndexByKey.emplace(groupedKey, groupedItems.size());
          if (inserted) {
            groupedItems.push_back(
                GroupedTaskItem{
                    .representative = task,
                    .cycleKey = groupedKey,
                    .candidates = {*task},
                }
            );
          } else {
            auto& grouped = groupedItems[it->second];
            grouped.candidates.push_back(*task);
            if (!grouped.representative->active && task->active) {
              grouped.representative = task;
            }
          }
        }

        renderedTasks.clear();
        renderedTasks.reserve(groupedItems.size());
        for (auto& grouped : groupedItems) {
          if (grouped.representative == nullptr) {
            continue;
          }
          renderedTasks.push_back(grouped.representative);
          badgeCountByHandle[grouped.representative->handleKey] = grouped.candidates.size();
          if (grouped.candidates.size() > 1) {
            cycleCandidatesByHandle[grouped.representative->handleKey] = std::move(grouped.candidates);
            cycleKeyByHandle[grouped.representative->handleKey] = grouped.cycleKey;
            cycleKeysThisFrame.insert(grouped.cycleKey);
          }
        }
      }

      const bool emptyWorkspace = renderedTasks.empty();
      const auto surfaceFill = colorSpecFromRole(ColorRole::SurfaceVariant, ws.workspace.active ? 0.52f : 0.18f);
      const auto borderColor = colorSpecFromRole(ColorRole::Primary, ws.workspace.active ? 0.65f : 0.16f);

      const float crossSize = std::round(tileSize + groupPad * 2.0f);

      float groupPadTop = groupPad;
      float groupPadRight = groupPad;
      float groupPadBottom = groupPad;
      float groupPadLeft = groupPad;
      std::optional<WorkspaceDiscSize> externalBadgeDisc;
      if (externalBadge) {
        externalBadgeDisc =
            measureWorkspaceDiscSize(renderer, ws.label, externalBadgeFontSize, badgeBase, m_contentScale, fontWeight);
        if (m_vertical) {
          *externalBadgeDisc = clampWorkspaceDiscToCrossLimit(*externalBadgeDisc, externalBadgeCrossLimit);
        }
        if (!tasks.empty() || m_vertical) {
          const float half =
              std::round(m_vertical ? externalBadgeDisc->height * 0.6f : externalBadgeDisc->width * 0.6f);
          if (m_vertical) {
            groupPadTop += half;
          } else {
            groupPadLeft += half;
          }
        }
      }
      if (groupedCrossInner > 0.0f) {
        if (m_vertical) {
          const float maxCrossPad = std::max(0.0f, (groupedCrossInner - tileWidthWithTitle) * 0.5f);
          groupPadLeft = std::min(groupPadLeft, maxCrossPad);
          groupPadRight = std::min(groupPadRight, maxCrossPad);
        } else {
          const float maxCrossPad = std::max(0.0f, (groupedCrossInner - tileSize) * 0.5f);
          groupPadTop = std::min(groupPadTop, maxCrossPad);
          groupPadBottom = std::min(groupPadBottom, maxCrossPad);
        }
      }

      auto group = ui::flex(
          m_vertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::Center,
              .gap = groupGap,
              .fill = m_workspaceGroupCapsule ? surfaceFill : clearColorSpec(),
              .radius = m_workspaceGroupCapsule ? resolvedBarCapsuleRadius(crossSize, crossSize) : 0.0f,
              .border = m_workspaceGroupCapsule ? borderColor : clearColorSpec(),
              .borderWidth = m_workspaceGroupCapsule ? Style::borderWidth * m_contentScale : 0.0f,
          }
      );
      group->setPadding(groupPadTop, groupPadRight, groupPadBottom, groupPadLeft);

      if (inlineBadge && m_showWorkspaceLabel) {
        const float inlineBadgeFontSize = std::round(Style::fontSizeCaption * 0.85f * m_contentScale);
        const float inlineBadgeHeight = std::round(std::max(10.0f, iconSize - (Style::spaceXs * m_contentScale)));
        WorkspaceDiscSize disc = measureWorkspaceDiscSize(
            renderer, ws.label, inlineBadgeFontSize, inlineBadgeHeight, m_contentScale, fontWeight
        );
        disc.height = inlineBadgeHeight;
        disc.width = std::round(std::max(inlineBadgeHeight, disc.width));
        if (m_vertical) {
          disc = clampWorkspaceDiscToCrossLimit(disc, externalBadgeCrossLimit);
        }
        group->addChild(createWorkspaceBadge(ws, disc, true));
      }

      if (emptyWorkspace) {
        if (!inlineBadge || !m_showWorkspaceLabel) {
          auto switcher = std::make_unique<InputArea>();
          switcher->setFrameSize(tileSize, tileSize);
          switcher->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
          switcher->setOnAxisHandler(workspaceAxisHandler);
          auto wsCopy = ws.workspace;
          wl_output* const wsHost = workspaceHostOutput(ws);
          switcher->setOnClick([this, wsCopy, wsHost](const InputArea::PointerData& data) {
            if (data.button == BTN_LEFT) {
              m_platform.activateWorkspace(wsHost, wsCopy);
            }
          });
          group->addChild(std::move(switcher));
        } else {
          // Inline badge is clickable; reserve tile cross extent so empty groups match occupied ones.
          if (m_vertical) {
            group->setMinWidth(groupPadLeft + tileSize + groupPadRight);
          } else {
            group->setMinHeight(groupPadTop + tileSize + groupPadBottom);
          }
        }
      } else {
        for (const auto* task : renderedTasks) {
          const auto cycleIt = cycleCandidatesByHandle.find(task->handleKey);
          const auto cycleKeyIt = cycleKeyByHandle.find(task->handleKey);
          const std::size_t badgeCount =
              badgeCountByHandle.contains(task->handleKey) ? badgeCountByHandle[task->handleKey] : 1;
          group->addChild(createTaskTile(
              *task, cycleIt != cycleCandidatesByHandle.end() ? cycleIt->second : std::vector<TaskModel>{},
              cycleKeyIt != cycleKeyByHandle.end() ? cycleKeyIt->second : std::string{}, badgeCount
          ));
        }
      }

      if (externalBadge) {
        // The group already has its content laid out; measure its real box (which
        // includes groupPad padding and the capsule border) so the badge is
        // centered against the actual group extent, not the tileSize approximation.
        const auto groupSize = group->measure(renderer, {});
        const WorkspaceDiscSize disc = *externalBadgeDisc;
        const auto badgePos = externalBadgePosition(
            m_workspaceLabelPlacement, m_vertical, groupSize.width, groupSize.height, disc.width, disc.height,
            groupOutlineInset
        );
        auto badge = createWorkspaceBadge(ws, disc, !emptyWorkspace);
        badge->setParticipatesInLayout(false);
        badge->setPosition(badgePos.left, badgePos.top);
        badge->layout(renderer);
        group->addChild(std::move(badge));
      }

      m_taskStrip->addChild(std::move(group));
    }
    std::erase_if(m_groupedAppCycleCursor, [&](const auto& item) { return !cycleKeysThisFrame.contains(item.first); });
    return;
  }
  m_taskStrip->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
  m_taskStrip->setGap(tileGap);
  m_groupedAppCycleCursor.clear();
  for (const auto& task : m_tasks) {
    m_taskStrip->addChild(createTaskTile(task));
  }
}

void TaskbarWidget::updateModels() {

  syncWorkspaceGroupingCapability();

  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    buildDesktopIconIndex();
  }

  const auto active = m_platform.activeToplevel();
  const auto* activeHandle = active.has_value() ? active->handle : nullptr;

  wl_output* const topFilter = toplevelOutputFilter();
  const auto assignmentMode = m_platform.taskbarAssignmentMode();
  std::vector<WorkspaceModel> nextWorkspaces;
  std::unordered_map<std::string, std::vector<std::string>> runningByWorkspace;
  std::vector<WorkspaceWindowAssignment> workspaceAssignments;

  {
    // Always load compositor workspace layout so flat-strip tile order can track window moves.
    nextWorkspaces.reserve(32);
    std::unordered_map<wl_output*, int> monitorOrdinal;
    int nextOrdinal = 1;
    for (const auto& wo : m_platform.outputs()) {
      if (wo.output == nullptr) {
        continue;
      }
      if (!m_showAllOutputs && wo.output != m_output) {
        continue;
      }
      monitorOrdinal.emplace(wo.output, nextOrdinal++);
    }

    for (const auto& wo : m_platform.outputs()) {
      if (wo.output == nullptr) {
        continue;
      }
      if (!m_showAllOutputs && wo.output != m_output) {
        continue;
      }
      const auto workspaces = m_platform.workspaces(wo.output);
      const auto displayKeys = m_platform.workspaceDisplayKeys(wo.output);
      const std::string keyPrefix = workspaceKeyPrefixForOutput(wo.output);
      nextWorkspaces.reserve(nextWorkspaces.size() + workspaces.size());
      for (std::size_t i = 0; i < workspaces.size(); ++i) {
        WorkspaceModel item{};
        item.workspace = workspaces[i];
        item.hostOutput = wo.output;
        const std::string fallbackLabel = workspaceLabel(item.workspace, i);
        const std::string label = compositors::isKde() && item.workspace.index > 0
            ? std::to_string(item.workspace.index)
            : (i < displayKeys.size() && !displayKeys[i].empty() ? displayKeys[i] : fallbackLabel);
        const std::string baseKey = compositors::isKde() && !item.workspace.id.empty()
            ? item.workspace.id
            : (i < displayKeys.size() && !displayKeys[i].empty() ? displayKeys[i] : fallbackLabel);
        item.key = keyPrefix + baseKey;
        if (useMultiOutputWorkspaceKeys()) {
          const auto ordIt = monitorOrdinal.find(wo.output);
          if (ordIt != monitorOrdinal.end()) {
            item.label = label + "\u00B7" + std::to_string(ordIt->second);
          } else {
            item.label = label;
          }
        } else {
          item.label = label;
        }
        nextWorkspaces.push_back(std::move(item));
      }
    }

    if (m_showAllOutputs) {
      for (const auto& wo : m_platform.outputs()) {
        if (wo.output == nullptr) {
          continue;
        }
        const std::string prefix = workspaceKeyPrefixForOutput(wo.output);
        const auto perApps = m_platform.appIdsByWorkspace(wo.output);
        for (const auto& [wsKey, apps] : perApps) {
          auto& bucket = runningByWorkspace[prefix + wsKey];
          bucket.insert(bucket.end(), apps.begin(), apps.end());
        }
        auto perAssign = m_platform.workspaceWindowAssignments(wo.output);
        for (auto& row : perAssign) {
          row.workspaceKey = prefix + row.workspaceKey;
          workspaceAssignments.push_back(std::move(row));
        }
      }
    } else {
      runningByWorkspace = m_platform.appIdsByWorkspace(m_output);
      workspaceAssignments = m_platform.workspaceWindowAssignments(m_output);
    }

    std::unordered_map<std::string, std::size_t> workspaceKeyToOrder;
    for (std::size_t i = 0; i < nextWorkspaces.size(); ++i) {
      workspaceKeyToOrder[nextWorkspaces[i].key] = i;
    }

    std::ranges::stable_sort(workspaceAssignments, [&](const auto& a, const auto& b) {
      if (a.workspaceKey != b.workspaceKey) {
        const auto itA = workspaceKeyToOrder.find(a.workspaceKey);
        const auto itB = workspaceKeyToOrder.find(b.workspaceKey);
        if (itA != workspaceKeyToOrder.end() && itB != workspaceKeyToOrder.end()) {
          return itA->second < itB->second;
        }
        return a.workspaceKey < b.workspaceKey;
      }
      if (a.x != b.x) {
        return a.x < b.x;
      }
      if (a.y != b.y) {
        return a.y < b.y;
      }
      return a.windowId < b.windowId;
    });
  }

  std::vector<std::string> running = m_platform.runningAppIds(topFilter);
  if (compositors::isHyprland() || compositors::isKde()) {
    std::unordered_set<std::string> seenApps(running.begin(), running.end());
    for (const auto& row : workspaceAssignments) {
      if (!row.appId.empty() && seenApps.insert(row.appId).second) {
        running.push_back(row.appId);
      }
    }
  }
  const auto resolvedRunning = app_identity::resolveRunningApps(running, desktopEntries());

  std::vector<TaskModel> nextTasks;
  std::unordered_set<std::uintptr_t> processedHandles;
  for (const auto& run : resolvedRunning) {
    const std::string idLower = run.runningLower;
    const std::string startupLower =
        !run.entry.startupWmClass.empty() ? toLower(run.entry.startupWmClass) : run.runningLower;
    const std::string nameLower = !run.entry.nameLower.empty() ? run.entry.nameLower : run.runningLower;

    const auto windows = m_platform.windowsForApp(idLower, startupLower, topFilter);
    for (const auto& window : windows) {
      const auto handleKey = taskHandleKey(window);
      if (handleKey == 0 || !processedHandles.insert(handleKey).second) {
        continue;
      }

      TaskModel task{};
      task.handleKey = handleKey;
      task.order = window.order;
      task.appId = !window.appId.empty() ? window.appId : run.runningAppId;
      task.idLower = idLower;
      task.startupWmClassLower = startupLower;
      task.nameLower = nameLower;
      task.appIdLower = toLower(task.appId);
      task.title = window.title;
      task.active = activeHandle != nullptr && activeHandle == window.handle;
      task.firstHandle = window.handle;
      if (!window.identifier.empty()) {
        task.workspaceWindowId = window.identifier;
      }
      task.iconPath = resolveIconPath(run.runningAppId, run.entry.icon);
      task.workspaceKey = {};
      nextTasks.push_back(std::move(task));
    }
  }

  if (compositors::isKde() && nextTasks.empty()) {
    for (const auto& assignment : workspaceAssignments) {
      if (assignment.appId.empty()) {
        continue;
      }
      const std::string idLower = toLower(assignment.appId);
      std::uintptr_t handleKey = 0;
      if (!assignment.windowId.empty()) {
        handleKey = static_cast<std::uintptr_t>(std::hash<std::string>{}(assignment.windowId));
        if (handleKey == 0) {
          handleKey = 1;
        }
      }
      if (handleKey == 0) {
        handleKey = static_cast<std::uintptr_t>(
            std::hash<std::string>{}(assignment.appId + "\n" + assignment.title + "\n" + assignment.workspaceKey)
        );
        if (handleKey == 0) {
          handleKey = 1;
        }
      }
      if (!processedHandles.insert(handleKey).second) {
        continue;
      }

      TaskModel task{};
      task.handleKey = handleKey;
      task.appId = assignment.appId;
      task.idLower = idLower;
      task.startupWmClassLower = idLower;
      task.nameLower = idLower;
      task.appIdLower = idLower;
      task.title = assignment.title;
      task.workspaceWindowId = assignment.windowId;
      task.workspaceKey = assignment.workspaceKey;
      task.iconPath = resolveIconPath(assignment.appId, {});
      nextTasks.push_back(std::move(task));
    }
  }

  std::ranges::stable_sort(nextTasks, [](const TaskModel& a, const TaskModel& b) {
    if (a.order != b.order) {
      return a.order < b.order;
    }
    return a.handleKey < b.handleKey;
  });

  if (compositors::isHyprland()) {
    const auto focusedCompositorWindowId = m_platform.focusedCompositorWindowId();
    for (auto& task : nextTasks) {
      ToplevelInfo toplevelInfo{};
      toplevelInfo.handle = task.firstHandle;
      if (task.firstHandle == nullptr) {
        toplevelInfo.extHandle = reinterpret_cast<ext_foreign_toplevel_handle_v1*>(task.handleKey);
      }
      if (const auto mappedId = m_platform.compositorWindowIdForToplevelInfo(toplevelInfo); mappedId.has_value()) {
        task.workspaceWindowId = *mappedId;
      } else if (toplevelInfo.extHandle != nullptr) {
        task.workspaceWindowId.clear();
      }
      if (!task.active
          && focusedCompositorWindowId.has_value()
          && !task.workspaceWindowId.empty()
          && task.workspaceWindowId == *focusedCompositorWindowId) {
        task.active = true;
      }
    }
  }

  if (compositors::isKde()) {
    const auto activeToplevel = m_platform.activeToplevel();
    for (auto& task : nextTasks) {
      if (!task.workspaceWindowId.empty()) {
        for (const auto& row : workspaceAssignments) {
          if (row.windowId == task.workspaceWindowId) {
            task.workspaceKey = row.workspaceKey;
            break;
          }
        }
      }
      if (task.workspaceWindowId.empty()) {
        for (const auto& row : workspaceAssignments) {
          if (row.title == task.title && toLower(row.appId) == toLower(task.appId)) {
            task.workspaceWindowId = row.windowId;
            task.workspaceKey = row.workspaceKey;
            break;
          }
        }
      }
      if (!task.active && activeToplevel.has_value()) {
        const bool titleMatch = !activeToplevel->title.empty() && task.title == activeToplevel->title;
        const bool appMatch = !activeToplevel->appId.empty() && toLower(task.appId) == toLower(activeToplevel->appId);
        const bool idMatch = !activeToplevel->identifier.empty()
            && !task.workspaceWindowId.empty()
            && task.workspaceWindowId == activeToplevel->identifier;
        if (idMatch || (appMatch && titleMatch)) {
          task.active = true;
        }
      }
    }
  }

  if (!workspaceAssignments.empty()) {
    if (assignmentMode == TaskbarAssignmentMode::WorkspaceOccurrenceTitle) {
      std::vector<TaskbarWindowCandidate> candidates;
      candidates.reserve(nextTasks.size());
      for (const auto& task : nextTasks) {
        TaskbarWindowCandidate candidate{};
        candidate.handleKey = task.handleKey;
        candidate.title = task.title;
        auto append = [&](const std::string& value) {
          if (value.empty()) {
            return;
          }
          if (!std::ranges::contains(candidate.appIds, value)) {
            candidate.appIds.push_back(value);
          }
        };
        append(task.appIdLower);
        append(task.idLower);
        append(task.startupWmClassLower);
        append(task.nameLower);
        candidates.push_back(std::move(candidate));
      }

      std::unordered_map<std::uintptr_t, WorkspaceWindow> assignedByHandle;
      if (m_showAllOutputs) {
        for (const auto& wo : m_platform.outputs()) {
          if (wo.output == nullptr) {
            continue;
          }
          const auto part = m_platform.assignTaskbarWindows(candidates, wo.output);
          const std::string prefix = workspaceKeyPrefixForOutput(wo.output);
          for (const auto& [handleKey, assigned] : part) {
            WorkspaceWindow copy = assigned;
            if (!prefix.empty()) {
              copy.workspaceKey = prefix + copy.workspaceKey;
            }
            assignedByHandle[handleKey] = std::move(copy);
          }
        }
      } else {
        assignedByHandle = m_platform.assignTaskbarWindows(candidates, m_output);
      }
      std::vector<bool> representedAssignments(workspaceAssignments.size(), false);

      for (auto& task : nextTasks) {
        const auto assignedIt = assignedByHandle.find(task.handleKey);
        if (assignedIt == assignedByHandle.end()) {
          continue;
        }

        const auto& assigned = assignedIt->second;
        task.workspaceKey = assigned.workspaceKey;
        task.workspaceWindowId = assigned.windowId;

        for (std::size_t assignmentIndex = 0; assignmentIndex < workspaceAssignments.size(); ++assignmentIndex) {
          if (representedAssignments[assignmentIndex]) {
            continue;
          }
          const auto& assignment = workspaceAssignments[assignmentIndex];
          if (assignment.workspaceKey != assigned.workspaceKey) {
            continue;
          }
          if (!assigned.windowId.empty() && assignment.windowId != assigned.windowId) {
            continue;
          }
          if (toLower(assignment.appId) != task.appIdLower
              && toLower(assignment.appId) != task.idLower
              && toLower(assignment.appId) != task.startupWmClassLower
              && toLower(assignment.appId) != task.nameLower) {
            continue;
          }
          if (!assigned.title.empty() && !assignment.title.empty() && assignment.title != assigned.title) {
            continue;
          }
          task.workspaceOrder = assignmentIndex;
          representedAssignments[assignmentIndex] = true;
          break;
        }
      }

      auto syntheticTaskKey = [](const WorkspaceWindowAssignment& assignment, std::size_t index) {
        const std::string seed = assignment.windowId.empty()
            ? assignment.workspaceKey + "\n" + assignment.appId + "\n" + assignment.title + "\n" + std::to_string(index)
            : assignment.windowId;
        std::uintptr_t value = static_cast<std::uintptr_t>(std::hash<std::string>{}(seed));
        if (value == 0) {
          value = static_cast<std::uintptr_t>(index + 1);
        }
        return value;
      };

      if (m_groupByWorkspace) {
        for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
          if (representedAssignments[i]) {
            continue;
          }

          const auto& assignment = workspaceAssignments[i];
          if (assignment.workspaceKey.empty() || assignment.appId.empty()) {
            continue;
          }

          TaskModel task{};
          task.handleKey = syntheticTaskKey(assignment, i);
          task.order = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + i;
          task.appId = assignment.appId;
          task.idLower = toLower(task.appId);
          task.startupWmClassLower = task.idLower;
          task.nameLower = task.idLower;
          task.appIdLower = task.idLower;
          task.title = assignment.title;
          task.iconPath = resolveIconPath(task.appId, {});
          task.workspaceKey = assignment.workspaceKey;
          task.workspaceWindowId = assignment.windowId;
          task.workspaceOrder = i;
          nextTasks.push_back(std::move(task));
        }
      }
    } else {
      std::unordered_map<std::uintptr_t, std::string> previousWorkspaceByHandle;
      std::unordered_map<std::uintptr_t, std::string> previousWorkspaceWindowByHandle;
      previousWorkspaceByHandle.reserve(m_tasks.size());
      previousWorkspaceWindowByHandle.reserve(m_tasks.size());
      for (const auto& task : m_tasks) {
        if (!task.workspaceKey.empty()) {
          previousWorkspaceByHandle[task.handleKey] = task.workspaceKey;
        }
        if (!task.workspaceWindowId.empty()) {
          previousWorkspaceWindowByHandle[task.handleKey] = task.workspaceWindowId;
        }
      }
      for (auto& task : nextTasks) {
        if (task.workspaceWindowId.empty()) {
          const auto previousWindow = previousWorkspaceWindowByHandle.find(task.handleKey);
          if (previousWindow != previousWorkspaceWindowByHandle.end()) {
            task.workspaceWindowId = previousWindow->second;
          }
        }
      }
      std::unordered_map<std::string, const WorkspaceModel*> workspaceByAnyKey;
      workspaceByAnyKey.reserve(std::max<std::size_t>(m_workspaces.size(), nextWorkspaces.size()) * 3);
      for (const auto& ws : nextWorkspaces) {
        workspaceByAnyKey.emplace(ws.key, &ws);
        if (!ws.workspace.id.empty()) {
          workspaceByAnyKey.emplace(ws.workspace.id, &ws);
        }
        if (!ws.workspace.name.empty()) {
          workspaceByAnyKey.emplace(ws.workspace.name, &ws);
        }
      }
      auto isTransientWorkspace = [&](const std::string& workspaceKey) {
        const auto it = workspaceByAnyKey.find(workspaceKey);
        if (it == workspaceByAnyKey.end() || it->second == nullptr) {
          return false;
        }
        const auto& workspace = it->second->workspace;
        return !workspace.active && !workspace.occupied;
      };

      std::vector<bool> used(workspaceAssignments.size(), false);
      auto matchesApp = [&](const TaskModel& task, const WorkspaceWindowAssignment& assignment) {
        const std::string assignmentAppLower = toLower(assignment.appId);
        return assignmentAppLower == task.appIdLower
            || assignmentAppLower == task.idLower
            || assignmentAppLower == task.startupWmClassLower
            || assignmentAppLower == task.nameLower;
      };

      auto assignMatch = [&](TaskModel& task, bool requireTitle,
                             const std::function<bool(const WorkspaceWindowAssignment&)>& extraPredicate) -> bool {
        for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
          if (used[i]) {
            continue;
          }
          const auto& assignment = workspaceAssignments[i];
          if (!matchesApp(task, assignment)) {
            continue;
          }
          if (requireTitle && assignment.title.empty()) {
            continue;
          }
          if (requireTitle && assignment.title != task.title) {
            continue;
          }
          const auto previous = previousWorkspaceByHandle.find(task.handleKey);
          if (previous != previousWorkspaceByHandle.end()
              && assignment.workspaceKey != previous->second
              && isTransientWorkspace(assignment.workspaceKey)) {
            continue;
          }
          if (!extraPredicate(assignment)) {
            continue;
          }
          task.workspaceKey = assignment.workspaceKey;
          task.workspaceWindowId = assignment.windowId;
          task.workspaceOrder = i;
          used[i] = true;
          return true;
        }
        return false;
      };

      for (auto& task : nextTasks) {
        const auto previous = previousWorkspaceWindowByHandle.find(task.handleKey);
        if (previous == previousWorkspaceWindowByHandle.end()) {
          continue;
        }
        for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
          if (used[i]) {
            continue;
          }
          const auto& assignment = workspaceAssignments[i];
          if (assignment.windowId != previous->second || !matchesApp(task, assignment)) {
            continue;
          }
          task.workspaceKey = assignment.workspaceKey;
          task.workspaceWindowId = assignment.windowId;
          task.workspaceOrder = i;
          used[i] = true;
          break;
        }
      }

      for (auto& task : nextTasks) {
        if (!task.workspaceKey.empty()) {
          continue;
        }
        const auto previous = previousWorkspaceByHandle.find(task.handleKey);
        if (previous == previousWorkspaceByHandle.end()) {
          continue;
        }
        (void)assignMatch(task, true, [&](const WorkspaceWindowAssignment& assignment) {
          return assignment.workspaceKey == previous->second;
        });
      }

      for (auto& task : nextTasks) {
        if (!task.workspaceKey.empty()) {
          continue;
        }
        const auto previous = previousWorkspaceByHandle.find(task.handleKey);
        if (previous == previousWorkspaceByHandle.end()) {
          continue;
        }
        (void)assignMatch(task, false, [&](const WorkspaceWindowAssignment& assignment) {
          return assignment.workspaceKey == previous->second;
        });
      }

      for (auto& task : nextTasks) {
        if (!task.workspaceKey.empty()) {
          continue;
        }
        (void)assignMatch(task, true, [](const WorkspaceWindowAssignment&) { return true; });
      }

      for (auto& task : nextTasks) {
        if (!task.workspaceKey.empty()) {
          continue;
        }

        std::optional<std::size_t> matchIndex;
        for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
          if (used[i]) {
            continue;
          }
          const auto& assignment = workspaceAssignments[i];
          if (!matchesApp(task, assignment)) {
            continue;
          }
          const auto previous = previousWorkspaceByHandle.find(task.handleKey);
          if (previous != previousWorkspaceByHandle.end()
              && assignment.workspaceKey != previous->second
              && isTransientWorkspace(assignment.workspaceKey)) {
            continue;
          }
          if (matchIndex.has_value()) {
            matchIndex = std::nullopt;
            break;
          }
          matchIndex = i;
        }

        if (matchIndex.has_value()) {
          task.workspaceKey = workspaceAssignments[*matchIndex].workspaceKey;
          task.workspaceWindowId = workspaceAssignments[*matchIndex].windowId;
          task.workspaceOrder = *matchIndex;
          used[*matchIndex] = true;
        }
      }

      for (auto& task : nextTasks) {
        if (task.workspaceKey.empty() || task.workspaceOrder != std::numeric_limits<std::uint64_t>::max()) {
          continue;
        }

        for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
          if (used[i]) {
            continue;
          }
          const auto& assignment = workspaceAssignments[i];
          const std::string assignmentAppLower = toLower(assignment.appId);
          if (assignmentAppLower != task.appIdLower
              && assignmentAppLower != task.idLower
              && assignmentAppLower != task.startupWmClassLower
              && assignmentAppLower != task.nameLower) {
            continue;
          }
          if (assignment.workspaceKey != task.workspaceKey) {
            continue;
          }

          task.workspaceOrder = i;
          task.workspaceWindowId = assignment.windowId;
          used[i] = true;
          break;
        }
      }

      // Rebuild workspaceOrder from assignment stream order every frame so
      // left/right reorders are reflected even when toplevel `order` is static.
      std::unordered_set<std::string> currentAssignmentWindowIds;
      currentAssignmentWindowIds.reserve(workspaceAssignments.size());
      for (const auto& assignment : workspaceAssignments) {
        if (!assignment.windowId.empty()) {
          currentAssignmentWindowIds.insert(assignment.windowId);
        }
      }
      for (auto& task : nextTasks) {
        task.workspaceOrder = std::numeric_limits<std::uint64_t>::max();
      }
      std::vector<bool> orderClaimed(nextTasks.size(), false);
      std::unordered_set<std::string> claimedWindowIds;
      for (std::size_t taskIndex = 0; taskIndex < nextTasks.size(); ++taskIndex) {
        auto& task = nextTasks[taskIndex];
        if (task.workspaceWindowId.empty()) {
          continue;
        }
        for (std::size_t assignmentIndex = 0; assignmentIndex < workspaceAssignments.size(); ++assignmentIndex) {
          const auto& assignment = workspaceAssignments[assignmentIndex];
          if (assignment.windowId != task.workspaceWindowId || !matchesApp(task, assignment)) {
            continue;
          }
          task.workspaceKey = assignment.workspaceKey;
          task.workspaceOrder = assignmentIndex;
          orderClaimed[taskIndex] = true;
          claimedWindowIds.insert(assignment.windowId);
          break;
        }
      }
      for (std::size_t assignmentIndex = 0; assignmentIndex < workspaceAssignments.size(); ++assignmentIndex) {
        const auto& assignment = workspaceAssignments[assignmentIndex];
        if (!assignment.windowId.empty() && claimedWindowIds.contains(assignment.windowId)) {
          continue;
        }
        const std::string assignmentAppLower = toLower(assignment.appId);

        auto appMatches = [&](const TaskModel& task) {
          return assignmentAppLower == task.appIdLower
              || assignmentAppLower == task.idLower
              || assignmentAppLower == task.startupWmClassLower
              || assignmentAppLower == task.nameLower;
        };

        auto tryClaim = [&](bool requireWorkspace, bool requireTitle) -> bool {
          for (std::size_t i = 0; i < nextTasks.size(); ++i) {
            auto& task = nextTasks[i];
            if (orderClaimed[i] || !appMatches(task)) {
              continue;
            }
            if (!task.workspaceWindowId.empty() && !currentAssignmentWindowIds.contains(task.workspaceWindowId)) {
              continue;
            }
            if (requireWorkspace && task.workspaceKey != assignment.workspaceKey) {
              continue;
            }
            if (requireTitle && !assignment.title.empty() && assignment.title != task.title) {
              continue;
            }
            if (task.workspaceKey.empty()) {
              task.workspaceKey = assignment.workspaceKey;
            }
            task.workspaceWindowId = assignment.windowId;
            task.workspaceOrder = assignmentIndex;
            orderClaimed[i] = true;
            if (!assignment.windowId.empty()) {
              claimedWindowIds.insert(assignment.windowId);
            }
            return true;
          }
          return false;
        };

        if (tryClaim(true, true)) {
          continue;
        }
        if (tryClaim(true, false)) {
          continue;
        }
        if (tryClaim(false, true)) {
          continue;
        }
        (void)tryClaim(false, false);
      }
    }

    if (compositors::isHyprland()) {
      auto syntheticTaskHandleKey = [](const WorkspaceWindowAssignment& assignment, std::size_t index) {
        const std::string seed = assignment.windowId.empty()
            ? assignment.workspaceKey + "\n" + assignment.appId + "\n" + assignment.title + "\n" + std::to_string(index)
            : assignment.windowId;
        std::uintptr_t value = static_cast<std::uintptr_t>(std::hash<std::string>{}(seed));
        if (value == 0) {
          value = static_cast<std::uintptr_t>(index + 1);
        }
        return value;
      };

      std::unordered_set<std::string> representedWindowIds;
      representedWindowIds.reserve(nextTasks.size());
      for (const auto& task : nextTasks) {
        if (!task.workspaceWindowId.empty()) {
          representedWindowIds.insert(task.workspaceWindowId);
        }
      }

      for (std::size_t i = 0; i < workspaceAssignments.size(); ++i) {
        const auto& assignment = workspaceAssignments[i];
        if (assignment.appId.empty()) {
          continue;
        }
        if (!assignment.windowId.empty()) {
          if (representedWindowIds.contains(assignment.windowId)) {
            continue;
          }
          if (m_platform.isCompositorWindowIdKnown(assignment.windowId)) {
            continue;
          }
        }

        TaskModel task{};
        task.handleKey = syntheticTaskHandleKey(assignment, i);
        task.order = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + i;
        task.appId = assignment.appId;
        task.idLower = toLower(task.appId);
        task.startupWmClassLower = task.idLower;
        task.nameLower = task.idLower;
        task.appIdLower = task.idLower;
        task.title = assignment.title;
        task.iconPath = resolveIconPath(task.appId, {});
        task.workspaceKey = assignment.workspaceKey;
        task.workspaceWindowId = assignment.windowId;
        task.workspaceOrder = i;
        nextTasks.push_back(std::move(task));
        if (!assignment.windowId.empty()) {
          representedWindowIds.insert(assignment.windowId);
        }
      }
    }
  }

  if (workspaceAssignments.empty() && !runningByWorkspace.empty()) {
    std::unordered_map<std::uintptr_t, std::string> workspaceByHandle;
    std::unordered_map<std::string, std::size_t> appOccurrence;
    for (const auto& ws : nextWorkspaces) {
      const auto byKey = runningByWorkspace.find(ws.key);
      const auto byName = runningByWorkspace.find(ws.workspace.name);
      const auto byId = runningByWorkspace.find(ws.workspace.id);
      const auto* list = byKey != runningByWorkspace.end()
          ? &byKey->second
          : (byName != runningByWorkspace.end() ? &byName->second
                                                : (byId != runningByWorkspace.end() ? &byId->second : nullptr));
      if (list == nullptr) {
        continue;
      }
      for (const auto& appId : *list) {
        const std::string appLower = toLower(appId);
        const std::string startupWmClassLower = toLower(appId);
        const auto windows = m_platform.windowsForApp(appLower, startupWmClassLower, topFilter);
        if (windows.empty()) {
          continue;
        }
        const std::size_t index = appOccurrence[appLower]++;
        if (index < windows.size()) {
          workspaceByHandle[reinterpret_cast<std::uintptr_t>(windows[index].handle)] = ws.key;
        }
      }
    }
    for (auto& task : nextTasks) {
      if (const auto it = workspaceByHandle.find(task.handleKey);
          task.workspaceKey.empty() && it != workspaceByHandle.end()) {
        task.workspaceKey = it->second;
      }
    }
  }

  std::unordered_map<std::uintptr_t, std::string> previousWorkspaceByHandle;
  previousWorkspaceByHandle.reserve(m_tasks.size());
  for (const auto& task : m_tasks) {
    if (!task.workspaceKey.empty()) {
      previousWorkspaceByHandle[task.handleKey] = task.workspaceKey;
    }
  }
  const bool hasStableWorkspaceWindowAssignments =
      std::ranges::any_of(workspaceAssignments, [](const WorkspaceWindowAssignment& assignment) {
        return !assignment.windowId.empty();
      });
  if (assignmentMode != TaskbarAssignmentMode::WorkspaceOccurrenceTitle && !hasStableWorkspaceWindowAssignments) {
    std::unordered_set<std::uintptr_t> seenHandles;
    seenHandles.reserve(nextTasks.size());
    for (auto& task : nextTasks) {
      seenHandles.insert(task.handleKey);
      const auto previous = previousWorkspaceByHandle.find(task.handleKey);
      if (previous == previousWorkspaceByHandle.end() || previous->second.empty() || task.workspaceKey.empty()) {
        m_pendingWorkspaceTransitions.erase(task.handleKey);
        continue;
      }
      if (task.workspaceKey == previous->second) {
        m_pendingWorkspaceTransitions.erase(task.handleKey);
        continue;
      }

      auto& pending = m_pendingWorkspaceTransitions[task.handleKey];
      if (pending.targetWorkspaceKey != task.workspaceKey) {
        pending.targetWorkspaceKey = task.workspaceKey;
        pending.votes = 1;
      } else if (pending.votes < 255) {
        ++pending.votes;
      }

      if (pending.votes < 2) {
        task.workspaceKey = previous->second;
      } else {
        m_pendingWorkspaceTransitions.erase(task.handleKey);
      }
    }

    for (auto it = m_pendingWorkspaceTransitions.begin(); it != m_pendingWorkspaceTransitions.end();) {
      if (!seenHandles.contains(it->first)) {
        it = m_pendingWorkspaceTransitions.erase(it);
      } else {
        ++it;
      }
    }
  } else {
    m_pendingWorkspaceTransitions.clear();
    for (auto& task : nextTasks) {
      if (!task.workspaceKey.empty()) {
        continue;
      }
      const auto previous = previousWorkspaceByHandle.find(task.handleKey);
      if (previous != previousWorkspaceByHandle.end() && !previous->second.empty()) {
        task.workspaceKey = previous->second;
      }
    }
  }

  if (m_onlyActiveWorkspace && !nextWorkspaces.empty()) {
    std::unordered_set<std::string> activeKeys;
    activeKeys.reserve(nextWorkspaces.size() * 3);
    for (const auto& wsm : nextWorkspaces) {
      if (wsm.workspace.active) {
        activeKeys.insert(wsm.key);
        if (!wsm.workspace.id.empty()) {
          activeKeys.insert(wsm.workspace.id);
        }
        if (!wsm.workspace.name.empty()) {
          activeKeys.insert(wsm.workspace.name);
        }
      }
    }
    if (!activeKeys.empty()) {
      std::erase_if(nextTasks, [&activeKeys](const TaskModel& t) {
        return !t.workspaceKey.empty() && !activeKeys.contains(t.workspaceKey);
      });
      if (m_groupByWorkspace) {
        std::erase_if(nextWorkspaces, [](const WorkspaceModel& wsm) { return !wsm.workspace.active; });
      }
    }
  }

  if (m_groupByWorkspace && m_hideEmptyWorkspaces && !nextWorkspaces.empty()) {
    const auto workspaceHasTask = [](const WorkspaceModel& wsm, const std::vector<TaskModel>& tasks) {
      for (const auto& t : tasks) {
        if (taskInWorkspaceGroup(t, wsm)) {
          return true;
        }
      }
      return false;
    };
    std::erase_if(nextWorkspaces, [&](const WorkspaceModel& wsm) {
      return !wsm.workspace.active && !workspaceHasTask(wsm, nextTasks);
    });
  }

  if (!m_groupByWorkspace) {
    nextWorkspaces.clear();
    std::ranges::stable_sort(nextTasks, [](const TaskModel& a, const TaskModel& b) {
      if (a.workspaceOrder != b.workspaceOrder) {
        return a.workspaceOrder < b.workspaceOrder;
      }
      if (a.order != b.order) {
        return a.order < b.order;
      }
      return a.handleKey < b.handleKey;
    });
  }

  if (modelsEqual(nextTasks, nextWorkspaces)) {
    m_tasks = std::move(nextTasks);
    m_workspaces = std::move(nextWorkspaces);
    return;
  }
  m_tasks = std::move(nextTasks);
  m_workspaces = std::move(nextWorkspaces);
  m_rebuildPending = true;
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
}

bool TaskbarWidget::onPointerEvent(const PointerEvent& event) {
  if (m_contextMenuPopup == nullptr || !m_contextMenuPopup->isOpen()) {
    return false;
  }
  const bool consumed = m_contextMenuPopup->onPointerEvent(event);
  if (!consumed && event.type == PointerEvent::Type::Button && event.state == 1) {
    m_contextMenuPopup->close();
    return true;
  }
  return consumed;
}

void TaskbarWidget::openTaskContextMenu(const TaskModel& task, InputArea& area) {
  auto* renderContext = PanelManager::instance().renderContext();
  if (renderContext == nullptr) {
    return;
  }

  wl_surface* pointerSurface = m_platform.lastPointerSurface();
  auto* layerSurface = m_platform.layerSurfaceFor(pointerSurface);
  if (layerSurface == nullptr) {
    return;
  }

  const auto windows = m_platform.windowsForApp(task.idLower, task.startupWmClassLower, toplevelOutputFilter());
  m_contextMenuHandles.clear();
  m_contextMenuHandles.reserve(windows.size());
  for (const auto& window : windows) {
    if (window.handle != nullptr) {
      m_contextMenuHandles.push_back(window.handle);
    }
  }
  m_contextMenuPrimaryHandle = task.firstHandle;

  std::vector<DesktopAction> entryActions;
  std::string entryAppName = task.idLower.empty() ? task.appId : task.idLower;
  std::string entryWorkingDir;
  bool entryTerminal = false;
  const auto& entriesIndex = desktopEntries();
  for (const auto& entry : entriesIndex) {
    if (entry.idLower == task.idLower
        || entry.idLower == task.appIdLower
        || entry.startupWmClassLower == task.idLower
        || entry.startupWmClassLower == task.startupWmClassLower
        || entry.nameLower == task.nameLower) {
      entryActions = entry.actions;
      entryAppName = entry.id.empty() ? entry.name : entry.id;
      entryWorkingDir = entry.workingDir;
      entryTerminal = entry.terminal;
      break;
    }
  }

  // IDs 0..N-1 => desktop actions, -1 => close single, -2 => close all.
  std::vector<ContextMenuControlEntry> entries;
  entries.reserve(entryActions.size() + 3);
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(entryActions.size()); ++i) {
    entries.push_back(
        ContextMenuControlEntry{
            .id = i,
            .label = entryActions[static_cast<std::size_t>(i)].name,
            .enabled = true,
            .separator = false,
            .hasSubmenu = false,
        }
    );
  }
  if (!m_contextMenuHandles.empty()) {
    if (!entries.empty()) {
      entries.push_back(
          ContextMenuControlEntry{.id = -3, .label = {}, .enabled = false, .separator = true, .hasSubmenu = false}
      );
    }
    entries.push_back(
        ContextMenuControlEntry{
            .id = -1,
            .label = i18n::tr("dock.actions.close"),
            .enabled = m_contextMenuPrimaryHandle != nullptr,
            .separator = false,
            .hasSubmenu = false,
        }
    );
    if (m_contextMenuHandles.size() > 1) {
      entries.push_back(
          ContextMenuControlEntry{
              .id = -2,
              .label = i18n::tr("dock.actions.close-all"),
              .enabled = true,
              .separator = false,
              .hasSubmenu = false,
          }
      );
    }
  }

  if (entries.empty()) {
    return;
  }

  if (m_contextMenuPopup == nullptr) {
    m_contextMenuPopup = std::make_unique<ContextMenuPopup>(m_platform.wayland(), *renderContext);
  }
  m_contextMenuPopup->setShadowConfig(m_configService.config().shell.shadow);
  m_contextMenuPopup->setOnActivate([this, entryActions, entryAppName, entryWorkingDir,
                                     entryTerminal](const ContextMenuControlEntry& entry) {
    if (entry.id >= 0) {
      const auto idx = static_cast<std::size_t>(entry.id);
      if (idx < entryActions.size()) {
        const auto& action = entryActions[idx];
        auto& platform = m_platform;
        auto& configService = m_configService;
        DeferredCall::callLater([action, appName = entryAppName, workingDir = entryWorkingDir, terminal = entryTerminal,
                                 &platform, &configService]() {
          std::string token;
          if (platform.hasXdgActivation()) {
            token = platform.requestActivationToken(nullptr);
          }
          (void)desktop_entry_launch::launchAction(
              action, appName, workingDir, terminal,
              desktop_entry_launch::LaunchOptions{
                  .activationToken = std::move(token),
                  .runAsSystemdService = configService.config().shell.launchAppsAsSystemdServices,
                  .customCommand = configService.config().shell.launchAppsCustomCommand,
              }
          );
        });
      }
      return;
    }
    if (entry.id == -1) {
      if (m_contextMenuPrimaryHandle != nullptr) {
        m_platform.closeToplevel(m_contextMenuPrimaryHandle);
      }
      return;
    }
    if (entry.id == -2) {
      for (auto* handle : m_contextMenuHandles) {
        if (handle != nullptr) {
          m_platform.closeToplevel(handle);
        }
      }
    }
  });

  float absX = 0.0f;
  float absY = 0.0f;
  Node::absolutePosition(&area, absX, absY);
  const float anchorInset = std::round(std::max(6.0f, Style::spaceSm * m_contentScale));
  float anchorX = absX + anchorInset;
  float anchorY = absY + anchorInset;
  float anchorW = std::max(1.0f, area.width() - (anchorInset * 2.0f));
  float anchorH = std::max(1.0f, area.height() - (anchorInset * 2.0f));

  constexpr float kTaskMenuWidth = 240.0f;
  const float menuWidth = kTaskMenuWidth * m_contentScale;
  const std::int32_t gap = std::max(2, static_cast<std::int32_t>(std::lround(Style::spaceMd * m_contentScale)));

  std::optional<ContextMenuPopupPlacement> placement;
  if (m_barPosition == "top") {
    anchorY = absY + area.height() + static_cast<float>(gap);
    anchorH = 1.0f;
  } else if (m_barPosition == "bottom") {
    // Mirror top: gap from the task tile edge, not the pointer (tray still uses pointer-centered icons).
    anchorX = absX;
    anchorY = absY;
    anchorW = area.width();
    anchorH = 1.0f;
    placement = ContextMenuPopupPlacement{
        .anchor = XDG_POSITIONER_ANCHOR_TOP,
        .gravity = XDG_POSITIONER_GRAVITY_TOP,
        .offsetX = 0,
        .offsetY = -gap,
        .chromeAttachment = popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center,
            .vertical = popup_chrome::VerticalAttachment::Bottom
        },
    };
  } else if (m_barPosition == "left") {
    anchorX = absX + area.width() + (menuWidth * 0.5f) + static_cast<float>(gap);
    anchorW = 1.0f;
  } else if (m_barPosition == "right") {
    anchorX = absX - (menuWidth * 0.5f) - static_cast<float>(gap);
    anchorW = 1.0f;
  }

  m_contextMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .menuWidth = menuWidth,
          .maxVisible = 12,
          .anchor =
              PopupAnchorRect{
                  .x = static_cast<std::int32_t>(std::round(anchorX)),
                  .y = static_cast<std::int32_t>(std::round(anchorY)),
                  .width = static_cast<std::int32_t>(std::round(anchorW)),
                  .height = static_cast<std::int32_t>(std::round(anchorH)),
              },
          .parent =
              PopupSurfaceParent{
                  .layerSurface = layerSurface,
                  .output = m_output,
                  .wlSurface = pointerSurface,
              },
          .placement = placement,
      }
  );
}

std::string TaskbarWidget::toLower(std::string value) { return StringUtils::toLower(std::move(value)); }

std::string TaskbarWidget::workspaceLabel(const Workspace& workspace, std::size_t index) {
  const auto parseLeadingNumber = [](const std::string& value) -> std::optional<std::size_t> {
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
      return std::nullopt;
    }
    std::size_t parsed = 0;
    std::size_t i = 0;
    while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
      parsed = parsed * 10 + static_cast<std::size_t>(value[i] - '0');
      ++i;
    }
    return parsed > 0 ? std::optional<std::size_t>(parsed) : std::nullopt;
  };

  if (const auto id = parseLeadingNumber(workspace.id); id.has_value()) {
    return std::to_string(*id);
  }
  if (const auto name = parseLeadingNumber(workspace.name); name.has_value()) {
    return std::to_string(*name);
  }
  if (!workspace.name.empty()) {
    return workspace.name;
  }
  if (!workspace.id.empty()) {
    return workspace.id;
  }
  if (!workspace.coordinates.empty()) {
    return std::to_string(static_cast<std::size_t>(workspace.coordinates.front()) + 1U);
  }
  return std::to_string(index + 1);
}

bool TaskbarWidget::modelsEqual(
    const std::vector<TaskModel>& tasks, const std::vector<WorkspaceModel>& workspaces
) const {
  if (tasks.size() != m_tasks.size() || workspaces.size() != m_workspaces.size()) {
    return false;
  }
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    if (tasks[i].appId != m_tasks[i].appId
        || tasks[i].iconPath != m_tasks[i].iconPath
        || tasks[i].active != m_tasks[i].active
        || tasks[i].firstHandle != m_tasks[i].firstHandle
        || tasks[i].workspaceKey != m_tasks[i].workspaceKey
        || tasks[i].order != m_tasks[i].order
        || tasks[i].workspaceOrder != m_tasks[i].workspaceOrder
        || (m_showWindowTitle && tasks[i].title != m_tasks[i].title)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const auto& a = workspaces[i].workspace;
    const auto& b = m_workspaces[i].workspace;
    if (a.id != b.id
        || a.name != b.name
        || a.active != b.active
        || a.urgent != b.urgent
        || a.occupied != b.occupied
        || workspaces[i].key != m_workspaces[i].key
        || workspaces[i].label != m_workspaces[i].label
        || workspaces[i].hostOutput != m_workspaces[i].hostOutput) {
      return false;
    }
  }
  return true;
}

void TaskbarWidget::buildDesktopIconIndex() {
  m_appIconsByLower.clear();
  const auto& entries = desktopEntries();
  for (const auto& entry : entries) {
    if (entry.icon.empty()) {
      continue;
    }
    if (!entry.id.empty()) {
      m_appIconsByLower[toLower(entry.id)] = entry.icon;
    }
    if (!entry.startupWmClass.empty()) {
      m_appIconsByLower[toLower(entry.startupWmClass)] = entry.icon;
    }
    if (!entry.nameLower.empty()) {
      m_appIconsByLower[entry.nameLower] = entry.icon;
    }
  }
  m_desktopEntriesVersion = desktopEntriesVersion();
}

std::string TaskbarWidget::resolveIconPath(const std::string& appId, const std::string& iconNameOrPath) {
  const int iconTargetSize = std::max(1, static_cast<int>(std::round(Style::baseGlyphSize * m_contentScale)));

  auto resolveIconName = [this, iconTargetSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }
    return m_iconResolver.resolve(name, iconTargetSize);
  };

  if (!iconNameOrPath.empty()) {
    if (const std::string primary = resolveIconName(iconNameOrPath); !primary.empty()) {
      return primary;
    }
  }

  if (appId.starts_with("steam_app_")) {
    if (const auto entry = app_identity::findDesktopEntry(appId, desktopEntries());
        entry.has_value() && !entry->icon.empty()) {
      if (const std::string steamIcon = resolveIconName(entry->icon); !steamIcon.empty()) {
        return steamIcon;
      }
    }
  }

  if (const auto internal = internal_apps::metadataForAppId(appId); internal.has_value()) {
    return internal->iconPath;
  }

  const std::string appIdLower = toLower(appId);
  const auto it = m_appIconsByLower.find(appIdLower);
  if (it != m_appIconsByLower.end()) {
    if (const std::string desktopIcon = resolveIconName(it->second); !desktopIcon.empty()) {
      return desktopIcon;
    }
  }
  if (!appId.empty()) {
    if (const std::string appIcon = resolveIconName(appId); !appIcon.empty()) {
      return appIcon;
    }
  }
  return m_iconResolver.resolve("application-x-executable", iconTargetSize);
}

bool TaskbarWidget::activeWorkspaceIndex(std::size_t& index) const {
  // Try to find the workspace of the globally active task first
  for (const auto& task : m_tasks) {
    if (task.active) {
      for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
        if (taskInWorkspaceGroup(task, m_workspaces[i])) {
          index = i;
          return true;
        }
      }
      break; // Found active task, but it doesn't belong to any workspace we have
    }
  }

  // Fallback to the active workspace on the current output
  for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
    if (m_workspaces[i].workspace.active && m_workspaces[i].hostOutput == m_output) {
      index = i;
      return true;
    }
  }

  // Fallback to any active workspace
  for (std::size_t i = 0; i < m_workspaces.size(); ++i) {
    if (m_workspaces[i].workspace.active) {
      index = i;
      return true;
    }
  }
  return false;
}

void TaskbarWidget::activateAdjacentWorkspace(int direction) {
  if (!m_groupByWorkspace || m_workspaces.empty() || direction == 0) {
    return;
  }

  std::size_t targetIndex = 0;
  std::size_t current = 0;
  if (!activeWorkspaceIndex(current)) {
    targetIndex = direction > 0 ? 0 : (m_workspaces.size() - 1);
  } else if (direction > 0) {
    if (current + 1 >= m_workspaces.size()) {
      return;
    }
    targetIndex = current + 1;
  } else {
    if (current == 0) {
      return;
    }
    targetIndex = current - 1;
  }

  const auto& targetWs = m_workspaces[targetIndex];
  m_platform.activateWorkspace(workspaceHostOutput(targetWs), targetWs.workspace);
}

void TaskbarWidget::activateAdjacentTask(int direction) {
  if (m_tasks.size() < 2 || direction == 0) {
    return;
  }

  const size_t activeTaskIndex =
      std::ranges::find_if(m_tasks, [](const TaskModel& t) { return t.active; }) - m_tasks.begin();
  if (activeTaskIndex >= m_tasks.size()) {
    return;
  }
  size_t newIndex = activeTaskIndex;
  if (direction > 0 && activeTaskIndex + 1 < m_tasks.size()) {
    ++newIndex;
  } else if (direction < 0 && activeTaskIndex > 0) {
    --newIndex;
  }
  if (newIndex == activeTaskIndex) {
    return;
  }
  const auto& targetTask = m_tasks[newIndex];
  m_platform.activateToplevel(targetTask.firstHandle);
}

wl_output* TaskbarWidget::toplevelOutputFilter() const noexcept { return m_showAllOutputs ? nullptr : m_output; }

bool TaskbarWidget::useMultiOutputWorkspaceKeys() const noexcept {
  if (!m_showAllOutputs) {
    return false;
  }
  std::size_t n = 0;
  for (const auto& wo : m_platform.outputs()) {
    if (wo.output != nullptr) {
      ++n;
    }
  }
  return n > 1;
}

std::string TaskbarWidget::workspaceKeyPrefixForOutput(wl_output* out) const {
  if (!useMultiOutputWorkspaceKeys()) {
    return {};
  }
  std::string connector;
  if (const auto* info = m_platform.findOutputByWl(out); info != nullptr) {
    connector = info->connectorName;
  }
  if (!connector.empty()) {
    return connector + '\x1e';
  }
  return "display\x1e";
}

wl_output* TaskbarWidget::workspaceHostOutput(const WorkspaceModel& model) const noexcept {
  return model.hostOutput != nullptr ? model.hostOutput : m_output;
}

ColorSpec TaskbarWidget::workspaceFillColor(const Workspace& workspace) const {
  if (workspace.active) {
    if (m_activeUsesFocusedColor) {
      return m_focusedColor;
    }
    return m_occupiedColor;
  }
  if (workspace.urgent) {
    return m_urgentColor;
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = m_emptyColor;
  color.alpha *= 0.55f;
  return color;
}

bool TaskbarWidget::isFocusedOutput() const { return m_platform.preferredInteractiveOutput() == m_output; }

ColorSpec TaskbarWidget::workspaceTextColor(const Workspace& workspace) const {
  if (workspace.urgent) {
    return m_minimal ? m_urgentColor : readableColorForFill(m_urgentColor);
  }
  if (!m_minimal) {
    return readableColorForFill(workspaceFillColor(workspace));
  }
  if (workspace.active) {
    if (m_activeUsesFocusedColor) {
      return m_focusedColor;
    }
    return m_occupiedColor;
  }
  if (workspace.occupied) {
    return m_occupiedColor;
  }
  ColorSpec color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  color.alpha *= 0.55f;
  return color;
}

ColorRole TaskbarWidget::onRoleForFill(ColorRole fill) {
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

ColorSpec TaskbarWidget::readableColorForFill(const ColorSpec& fill) {
  if (fill.role.has_value()) {
    return colorSpecFromRole(onRoleForFill(*fill.role));
  }
  return fixedColorSpec(readableTextColorForBackground(resolveColorSpec(fill)));
}
