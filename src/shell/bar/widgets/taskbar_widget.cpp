#include "shell/bar/widgets/taskbar_widget.h"

#include "compositors/compositor_detect.h"
#include "compositors/workspace_backend.h"
#include "core/deferred_call.h"
#include "core/process.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "shell/panel/panel_manager.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/internal_app_metadata.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_seat.h"
#include "wayland/wayland_toplevels.h"

struct ext_foreign_toplevel_handle_v1;
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

  [[nodiscard]] float externalBadgeMainOverhang(WorkspaceLabelPlacement placement, float discMain) {
    if (placement == WorkspaceLabelPlacement::Centered) {
      return discMain * 0.5f;
    }
    return discMain * 0.32f;
  }

  [[nodiscard]] float externalBadgeCrossOverhang(WorkspaceLabelPlacement placement, float discCross) {
    if (placement == WorkspaceLabelPlacement::Centered) {
      return discCross * 0.5f;
    }
    return discCross * 0.22f;
  }

  [[nodiscard]] float
  externalBadgeTileMainStart(WorkspaceLabelPlacement placement, float discMain, float groupPad, float groupGap) {
    if (placement == WorkspaceLabelPlacement::Centered) {
      return std::round(discMain * 0.5f + groupGap);
    }
    constexpr float kCornerInsideMainFraction = 1.0f - 0.32f;
    return std::round(std::max(groupPad, discMain * kCornerInsideMainFraction + groupGap));
  }

  [[nodiscard]] ExternalBadgePosition externalBadgePosition(
      WorkspaceLabelPlacement placement, bool vertical, float groupWidth, float groupHeight, float badgeWidth,
      float badgeHeight, float outlineInset
  ) {
    const float cornerLeft = std::round(badgeWidth * -0.32f);
    const float cornerTop = std::round(badgeHeight * -0.22f);
    if (placement != WorkspaceLabelPlacement::Centered) {
      return {cornerLeft, cornerTop};
    }
    if (vertical) {
      return {centeredOffset(groupWidth, badgeWidth, outlineInset, false), std::round(-badgeHeight * 0.5f)};
    }
    return {std::round(-badgeWidth * 0.5f), centeredOffset(groupHeight, badgeHeight, outlineInset, false)};
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

} // namespace

TaskbarWidget::TaskbarWidget(
    CompositorPlatform& platform, wl_output* output, bool groupByWorkspace, bool showAllOutputs,
    bool onlyActiveWorkspace, bool showWorkspaceLabel, WorkspaceLabelPlacement workspaceLabelPlacement,
    bool hideEmptyWorkspaces, bool workspaceGroupCapsule, ColorSpec focusedColor, ColorSpec occupiedColor,
    ColorSpec emptyColor, std::string barPosition, ShellConfig::ShadowConfig shadowConfig
)
    : m_platform(platform), m_output(output), m_groupByWorkspace(groupByWorkspace), m_showAllOutputs(showAllOutputs),
      m_onlyActiveWorkspace(onlyActiveWorkspace), m_showWorkspaceLabel(showWorkspaceLabel),
      m_workspaceLabelPlacement(workspaceLabelPlacement), m_hideEmptyWorkspaces(hideEmptyWorkspaces),
      m_workspaceGroupCapsule(workspaceGroupCapsule), m_focusedColor(std::move(focusedColor)),
      m_occupiedColor(std::move(occupiedColor)), m_emptyColor(std::move(emptyColor)),
      m_barPosition(std::move(barPosition)), m_shadowConfig(std::move(shadowConfig)) {
  buildDesktopIconIndex();
}

TaskbarWidget::~TaskbarWidget() = default;

bool TaskbarWidget::taskInWorkspaceGroup(const TaskModel& task, const WorkspaceModel& ws) {
  return !task.workspaceKey.empty() && task.workspaceKey == ws.key;
}

void TaskbarWidget::create() {
  auto container = std::make_unique<InputArea>();
  container->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_groupByWorkspace) {
      return false;
    }

    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && data.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return false;
    }

    float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f && data.axisValue120 != 0) {
      delta = static_cast<float>(data.axisValue120) / 120.0f;
    }
    if (delta == 0.0f && data.axisDiscrete != 0) {
      delta = static_cast<float>(data.axisDiscrete);
    }
    if (delta == 0.0f) {
      return false;
    }
    activateAdjacentWorkspace(delta > 0.0f ? 1 : -1);
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

void TaskbarWidget::doUpdate(Renderer& renderer) {
  (void)renderer;
  updateModels();
}

void TaskbarWidget::rebuild(Renderer& renderer) {
  if (m_taskStrip == nullptr) {
    return;
  }
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
  const float iconSize = std::round(Style::barGlyphSize * m_contentScale);
  const float tilePadding = Style::spaceXs * 0.35f * m_contentScale;
  const float tileSize = std::round(iconSize + tilePadding * 2.0f);
  const float groupBorderInset = Style::borderWidth * m_contentScale;
  const float groupOutlineInset = m_workspaceGroupCapsule ? groupBorderInset : 0.0f;
  const FontWeight fontWeight = labelFontWeight();
  const auto workspaceAxisHandler = [this](const InputArea::PointerData& data) -> bool {
    if (!m_groupByWorkspace) {
      return false;
    }
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL && data.axis != WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      return false;
    }

    float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f && data.axisValue120 != 0) {
      delta = static_cast<float>(data.axisValue120) / 120.0f;
    }
    if (delta == 0.0f && data.axisDiscrete != 0) {
      delta = static_cast<float>(data.axisDiscrete);
    }
    if (delta == 0.0f) {
      return false;
    }

    activateAdjacentWorkspace(delta > 0.0f ? 1 : -1);
    return true;
  };
  auto createTaskTile = [&](const TaskModel& task) {
    auto area = std::make_unique<InputArea>();
    area->setFrameSize(tileSize, tileSize);
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

    if (task.firstHandle != nullptr || !task.workspaceWindowId.empty() || clickWorkspace.has_value()) {
      auto* areaPtr = area.get();
      area->setOnClick([this, task, areaPtr, handle = task.firstHandle, windowId = task.workspaceWindowId,
                        clickWorkspace, taskWsHost](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          if (handle != nullptr) {
            m_platform.activateToplevel(handle);
            return;
          }
          if (!windowId.empty()) {
            m_platform.focusCompositorWindow(windowId);
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

    const bool groupedHorizontalPill = m_groupByWorkspace && !m_vertical;
    if (!task.iconPath.empty()) {
      const float iconInsetX = centeredOffset(tileSize, iconSize);
      // Match symmetric pill centering; default odd-pixel bias sits icons ~1px high in grouped capsules.
      const float iconInsetY = centeredOffset(tileSize, iconSize, 0.0f, !groupedHorizontalPill);
      auto image = ui::image({
          .fit = ImageFit::Contain,
          .width = iconSize,
          .height = iconSize,
      });
      image->setPosition(iconInsetX, iconInsetY);
      image->setSourceFile(renderer, task.iconPath, static_cast<int>(std::round(48.0f * m_contentScale)), true);
      area->addChild(std::move(image));
    } else {
      auto glyph = ui::glyph({
          .glyph = "apps",
          .glyphSize = iconSize,
      });
      glyph->measure(renderer);
      glyph->setPosition(
          centeredOffset(tileSize, glyph->width()),
          centeredOffset(tileSize, glyph->height(), 0.0f, !groupedHorizontalPill)
      );
      area->addChild(std::move(glyph));
    }

    if (task.active) {
      const float d = std::max(4.0f, std::round(Style::barGlyphSize * 0.32f * m_contentScale));
      const float bottomInset = 0.25f * m_contentScale;
      auto indicator = ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = resolvedBarCapsuleRadius(d, d),
          .width = d,
          .height = d,
      });
      indicator->setPosition(std::round((tileSize - d) * 0.5f), std::round(tileSize - d - bottomInset));
      area->addChild(std::move(indicator));
    }
    return area;
  };

  if (m_groupByWorkspace && !m_workspaces.empty()) {
    const float groupGap = Style::spaceXs * m_contentScale;
    const float groupPad = Style::spaceXs * m_contentScale;
    const float groupPadMain = Style::spaceXs * 0.55f * m_contentScale;
    const float groupPadCross = Style::spaceXs * 0.35f * m_contentScale;
    const bool inlineBadge = m_showWorkspaceLabel && m_workspaceLabelPlacement == WorkspaceLabelPlacement::Inside;
    const bool externalBadge = m_showWorkspaceLabel && !inlineBadge;
    const float badgeBase = std::round(std::max(11.0f, Style::barGlyphSize * 0.72f) * m_contentScale);
    const float externalBadgeFontSize = std::round(Style::fontSizeCaption * 0.72f * m_contentScale);

    float stripGap = groupGap;
    if (externalBadge && !m_workspaceGroupCapsule) {
      float maxMainOverhang = 0.0f;
      for (const auto& wsm : m_workspaces) {
        const auto measuredDisc =
            measureWorkspaceDiscSize(renderer, wsm.label, externalBadgeFontSize, badgeBase, m_contentScale, fontWeight);
        const float measuredMain = m_vertical ? measuredDisc.height : measuredDisc.width;
        maxMainOverhang = std::max(maxMainOverhang, externalBadgeMainOverhang(m_workspaceLabelPlacement, measuredMain));
      }
      stripGap = std::round(std::max(groupGap, maxMainOverhang + groupGap));
    }
    m_taskStrip->setGap(stripGap);
    m_taskStrip->setPadding(0.0f, 0.0f, 0.0f, 0.0f);

    const auto styleWorkspaceDisc = [this](Box& badge, float width, float height, const Workspace& workspace) {
      badge.setFrameSize(width, height);
      badge.setRadius(resolvedBarCapsuleRadius(width, height));
      badge.setFill(workspaceFillColor(workspace));
      badge.clearBorder();
    };

    auto createWorkspaceBadgeTile = [&](const WorkspaceModel& ws) {
      auto area = std::make_unique<InputArea>();
      area->setFrameSize(tileSize, tileSize);
      area->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      area->setOnAxisHandler(workspaceAxisHandler);
      auto wsCopy = ws.workspace;
      wl_output* const wsHost = workspaceHostOutput(ws);
      area->setOnClick([this, wsCopy, wsHost](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          m_platform.activateWorkspace(wsHost, wsCopy);
        }
      });

      const bool groupedHorizontalPill = m_groupByWorkspace && !m_vertical;
      const float inlineBadgeFontSize = std::round(Style::fontSizeCaption * 0.85f * m_contentScale);
      WorkspaceDiscSize disc =
          measureWorkspaceDiscSize(renderer, ws.label, inlineBadgeFontSize, iconSize, m_contentScale, fontWeight);
      disc.height = iconSize;
      disc.width = std::round(std::max(iconSize, disc.width));
      const float badgeX = centeredOffset(tileSize, disc.width);
      const float badgeY = centeredOffset(tileSize, disc.height, 0.0f, !groupedHorizontalPill);

      auto badge = ui::box();
      badge->setPosition(badgeX, badgeY);
      styleWorkspaceDisc(*badge, disc.width, disc.height, ws.workspace);
      auto* badgePtr = static_cast<Box*>(area->addChild(std::move(badge)));

      const float badgeFontSize =
          fitBadgeFontSize(renderer, ws.label, disc.width, disc.height, m_contentScale, fontWeight);
      auto badgeText = ui::label({
          .text = ws.label,
          .fontSize = badgeFontSize,
          .color = workspaceTextColor(ws.workspace),
          .fontWeight = fontWeight,
      });
      badgeText->measure(renderer);
      badgeText->setPosition(
          std::round((disc.width - badgeText->width()) * 0.5f), std::round((disc.height - badgeText->height()) * 0.5f)
      );
      badgePtr->addChild(std::move(badgeText));
      return area;
    };

    auto addExternalWorkspaceBadge = [&](const WorkspaceModel& ws, Box* badgeParent, float badgeLayoutWidth,
                                         float badgeLayoutHeight, const WorkspaceDiscSize& disc, bool emptyWorkspace,
                                         float badgeOriginMain, float badgeOriginCross) {
      const auto badgePos = externalBadgePosition(
          m_workspaceLabelPlacement, m_vertical, badgeLayoutWidth, badgeLayoutHeight, disc.width, disc.height,
          groupOutlineInset
      );
      auto badgeHit = std::make_unique<InputArea>();
      badgeHit->setFrameSize(disc.width, disc.height);
      if (m_vertical) {
        badgeHit->setPosition(badgeOriginCross + badgePos.left, badgeOriginMain + badgePos.top);
      } else {
        badgeHit->setPosition(badgeOriginMain + badgePos.left, badgeOriginCross + badgePos.top);
      }
      badgeHit->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      badgeHit->setOnAxisHandler(workspaceAxisHandler);
      auto wsForBadge = ws.workspace;
      wl_output* const badgeHost = workspaceHostOutput(ws);
      badgeHit->setOnClick([this, wsForBadge, badgeHost](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          m_platform.activateWorkspace(badgeHost, wsForBadge);
        }
      });

      auto badge = ui::box();
      badge->setPosition(0.0f, 0.0f);
      styleWorkspaceDisc(*badge, disc.width, disc.height, ws.workspace);
      auto* badgePtr = static_cast<Box*>(badgeHit->addChild(std::move(badge)));

      const float badgeFontSize =
          fitBadgeFontSize(renderer, ws.label, disc.width, disc.height, m_contentScale, fontWeight);
      auto badgeText = ui::label({
          .text = ws.label,
          .fontSize = badgeFontSize,
          .color = workspaceTextColor(ws.workspace),
          .fontWeight = fontWeight,
      });
      badgeText->measure(renderer);
      badgeText->setPosition(
          std::round((disc.width - badgeText->width()) * 0.5f), std::round((disc.height - badgeText->height()) * 0.5f)
      );
      badgePtr->addChild(std::move(badgeText));
      if (emptyWorkspace) {
        badgeHit->setHitTestVisible(false);
      }
      badgeParent->addChild(std::move(badgeHit));
    };

    for (std::size_t groupIndex = 0; groupIndex < m_workspaces.size(); ++groupIndex) {
      const auto& ws = m_workspaces[groupIndex];
      std::vector<const TaskModel*> tasks;
      for (const auto& task : m_tasks) {
        if (taskInWorkspaceGroup(task, ws)) {
          tasks.push_back(&task);
        }
      }
      std::stable_sort(tasks.begin(), tasks.end(), [](const TaskModel* lhs, const TaskModel* rhs) {
        if (lhs->workspaceOrder != rhs->workspaceOrder) {
          return lhs->workspaceOrder < rhs->workspaceOrder;
        }
        if (lhs->order != rhs->order) {
          return lhs->order < rhs->order;
        }
        return lhs->handleKey < rhs->handleKey;
      });

      const bool emptyWorkspace = tasks.empty();
      WorkspaceDiscSize disc{};
      if (externalBadge) {
        disc =
            measureWorkspaceDiscSize(renderer, ws.label, externalBadgeFontSize, badgeBase, m_contentScale, fontWeight);
      }

      const float discMain = externalBadge ? (m_vertical ? disc.height : disc.width) : 0.0f;
      const float discCross = externalBadge ? (m_vertical ? disc.width : disc.height) : 0.0f;
      const bool externalInsetCapsule = externalBadge && m_workspaceGroupCapsule;
      const float mainOverhang = externalBadgeMainOverhang(m_workspaceLabelPlacement, discMain);
      const float crossOverhang = externalBadgeCrossOverhang(m_workspaceLabelPlacement, discCross);
      const float groupOuterLead =
          externalInsetCapsule ? std::round(std::max(groupPad, mainOverhang + (groupIndex > 0 ? groupGap : 0.0f)))
                               : 0.0f;
      const bool needsOuterCrossLead =
          externalInsetCapsule && (m_workspaceLabelPlacement == WorkspaceLabelPlacement::Corner ||
                                   (m_workspaceLabelPlacement == WorkspaceLabelPlacement::Centered && m_vertical));
      const float groupOuterCrossLead = needsOuterCrossLead ? std::round(std::max(groupPadCross, crossOverhang)) : 0.0f;

      float tileMain = inlineBadge ? groupPadMain : groupPad;
      if (externalBadge) {
        tileMain = externalBadgeTileMainStart(m_workspaceLabelPlacement, discMain, groupPad, groupGap);
      }

      const std::size_t inlineSlotCount =
          m_showWorkspaceLabel ? (emptyWorkspace ? 1U : tasks.size() + 1) : (emptyWorkspace ? 0U : tasks.size());
      const float taskCount = std::max(1.0f, static_cast<float>(tasks.size()));
      const float externalGapCount = tasks.empty() ? 0.0f : (taskCount - 1.0f);
      const float runLength =
          inlineBadge ? (inlineSlotCount > 0
                             ? (tileSize * static_cast<float>(inlineSlotCount)) +
                                   (groupGap * (inlineSlotCount > 1 ? static_cast<float>(inlineSlotCount - 1) : 0.0f))
                             : tileSize)
                      : (tileSize * taskCount) + (groupGap * externalGapCount);
      const float innerMainTotal = inlineBadge ? (groupPadMain * 2.0f + runLength) : (tileMain + groupPad + runLength);
      const float innerCrossSize =
          inlineBadge ? std::round(tileSize + (groupPadCross * 2.0f))
                      : (m_vertical ? std::round(tileSize + (groupPadCross * 2.0f)) : std::round(tileSize));

      float groupWidth = m_vertical ? innerCrossSize : innerMainTotal;
      float groupHeight = m_vertical ? innerMainTotal : innerCrossSize;
      if (externalInsetCapsule) {
        if (m_vertical) {
          groupHeight = std::round(groupOuterLead + innerMainTotal);
          groupWidth = std::round(innerCrossSize + (groupOuterCrossLead * 2.0f));
        } else {
          groupWidth = std::round(groupOuterLead + innerMainTotal);
          groupHeight = std::round(innerCrossSize + groupOuterCrossLead);
        }
      }
      if (emptyWorkspace && !m_showWorkspaceLabel) {
        groupWidth =
            m_vertical ? std::round(tileSize + (groupPadCross * 2.0f)) : std::round(tileSize + (groupPadMain * 2.0f));
        groupHeight =
            m_vertical ? std::round(tileSize + (groupPadMain * 2.0f)) : std::round(tileSize + (groupPadCross * 2.0f));
      }

      const float contentWidth = externalInsetCapsule ? (m_vertical ? innerCrossSize : innerMainTotal) : groupWidth;
      const float contentHeight = externalInsetCapsule ? (m_vertical ? innerMainTotal : innerCrossSize) : groupHeight;
      const float tileCrossExtent = m_vertical ? contentWidth : contentHeight;
      const float inlineGroupCross = inlineBadge ? innerCrossSize : tileCrossExtent;
      const float tileCross = inlineBadge
                                  ? centeredOffset(inlineGroupCross, tileSize, groupOutlineInset, true)
                                  : (m_vertical ? centeredOffset(tileCrossExtent, tileSize, groupOutlineInset, false)
                                                : centeredOffset(tileCrossExtent, tileSize, groupOutlineInset, true));
      const float contentOriginMain = externalInsetCapsule ? groupOuterLead : 0.0f;
      const float contentOriginCross = externalInsetCapsule ? groupOuterCrossLead : 0.0f;

      auto group = ui::box({
          .width = groupWidth,
          .height = groupHeight,
      });
      const auto surfaceFill = colorSpecFromRole(ColorRole::SurfaceVariant, ws.workspace.active ? 0.52f : 0.18f);
      const auto borderColor = colorSpecFromRole(ColorRole::Primary, ws.workspace.active ? 0.65f : 0.16f);
      if (m_workspaceGroupCapsule) {
        if (externalInsetCapsule) {
          group->setFill(clearColorSpec());
          group->clearBorder();
        } else {
          group->setFill(surfaceFill);
          group->setBorder(borderColor, Style::borderWidth);
        }
        group->setRadius(resolvedBarCapsuleRadius(groupWidth, groupHeight));
      } else {
        group->setFill(clearColorSpec());
        group->clearBorder();
        group->setRadius(0.0f);
      }
      auto* groupPtr = static_cast<Box*>(m_taskStrip->addChild(std::move(group)));

      Box* contentPtr = groupPtr;
      if (externalInsetCapsule) {
        auto inner = ui::box({
            .width = contentWidth,
            .height = contentHeight,
        });
        inner->setPosition(
            m_vertical ? contentOriginCross : contentOriginMain, m_vertical ? contentOriginMain : contentOriginCross
        );
        inner->setFill(surfaceFill);
        inner->setBorder(borderColor, Style::borderWidth);
        inner->setRadius(resolvedBarCapsuleRadius(contentWidth, contentHeight));
        contentPtr = static_cast<Box*>(groupPtr->addChild(std::move(inner)));
      }

      if (emptyWorkspace && !m_showWorkspaceLabel) {
        auto switcher = std::make_unique<InputArea>();
        switcher->setFrameSize(groupWidth, groupHeight);
        switcher->setPosition(0.0f, 0.0f);
        switcher->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        switcher->setOnAxisHandler(workspaceAxisHandler);
        auto wsCopy = ws.workspace;
        wl_output* const wsHost = workspaceHostOutput(ws);
        switcher->setOnClick([this, wsCopy, wsHost](const InputArea::PointerData& data) {
          if (data.button == BTN_LEFT) {
            m_platform.activateWorkspace(wsHost, wsCopy);
          }
        });
        groupPtr->addChild(std::move(switcher));
      } else if (inlineBadge) {
        for (std::size_t slot = 0; slot < inlineSlotCount; ++slot) {
          const float tileOffset = (tileSize + groupGap) * static_cast<float>(slot);
          std::unique_ptr<Node> tile;
          if (m_showWorkspaceLabel && slot == 0) {
            tile = createWorkspaceBadgeTile(ws);
          } else {
            const std::size_t taskIndex = m_showWorkspaceLabel ? slot - 1 : slot;
            tile = createTaskTile(*tasks[taskIndex]);
          }
          if (m_vertical) {
            tile->setPosition(tileCross, std::round(tileMain + tileOffset));
          } else {
            tile->setPosition(std::round(tileMain + tileOffset), tileCross);
          }
          contentPtr->addChild(std::move(tile));
        }
      } else {
        if (emptyWorkspace) {
          auto switcher = std::make_unique<InputArea>();
          switcher->setFrameSize(contentWidth, contentHeight);
          switcher->setPosition(0.0f, 0.0f);
          switcher->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
          switcher->setOnAxisHandler(workspaceAxisHandler);
          auto wsCopy = ws.workspace;
          wl_output* const wsHost = workspaceHostOutput(ws);
          switcher->setOnClick([this, wsCopy, wsHost](const InputArea::PointerData& data) {
            if (data.button == BTN_LEFT) {
              m_platform.activateWorkspace(wsHost, wsCopy);
            }
          });
          contentPtr->addChild(std::move(switcher));
        }
        for (std::size_t i = 0; i < tasks.size(); ++i) {
          const float tileOffset = (tileSize + groupGap) * static_cast<float>(i);
          auto tile = createTaskTile(*tasks[i]);
          if (m_vertical) {
            tile->setPosition(tileCross, std::round(tileMain + tileOffset));
          } else {
            tile->setPosition(std::round(tileMain + tileOffset), tileCross);
          }
          contentPtr->addChild(std::move(tile));
        }
        if (externalBadge) {
          Box* badgeParent = externalInsetCapsule ? groupPtr : contentPtr;
          const float badgeOriginMain = externalInsetCapsule ? contentOriginMain : 0.0f;
          const float badgeOriginCross = externalInsetCapsule ? contentOriginCross : 0.0f;
          addExternalWorkspaceBadge(
              ws, badgeParent, contentWidth, contentHeight, disc, emptyWorkspace, badgeOriginMain, badgeOriginCross
          );
        }
      }
    }
    return;
  }

  m_taskStrip->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
  m_taskStrip->setGap(Style::spaceSm * m_contentScale);

  for (const auto& task : m_tasks) {
    m_taskStrip->addChild(createTaskTile(task));
  }
}

void TaskbarWidget::updateModels() {
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
        const std::string baseKey =
            i < displayKeys.size() && !displayKeys[i].empty() ? displayKeys[i] : workspaceLabel(item.workspace, i);
        item.key = keyPrefix + baseKey;
        if (useMultiOutputWorkspaceKeys()) {
          const auto ordIt = monitorOrdinal.find(wo.output);
          if (ordIt != monitorOrdinal.end()) {
            item.label = baseKey + "\u00B7" + std::to_string(ordIt->second);
          } else {
            item.label = baseKey;
          }
        } else {
          item.label = baseKey;
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

    std::stable_sort(workspaceAssignments.begin(), workspaceAssignments.end(), [&](const auto& a, const auto& b) {
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
  if (compositors::isHyprland()) {
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
    const std::string idLower = !run.entry.id.empty() ? toLower(run.entry.id) : run.runningLower;
    const std::string startupLower = toLower(run.entry.startupWmClass);
    const std::string nameLower = !run.entry.nameLower.empty() ? run.entry.nameLower : run.runningLower;
    const std::string appId = !run.entry.id.empty() ? run.entry.id : run.runningAppId;

    const auto windows = m_platform.windowsForApp(idLower, startupLower, topFilter);
    for (const auto& window : windows) {
      const auto handleKey = window.handle != nullptr ? reinterpret_cast<std::uintptr_t>(window.handle)
                                                      : reinterpret_cast<std::uintptr_t>(window.extHandle);
      if (handleKey == 0 || !processedHandles.insert(handleKey).second) {
        continue;
      }

      TaskModel task{};
      task.handleKey = handleKey;
      task.order = window.order;
      task.appId = !window.appId.empty() ? window.appId : appId;
      task.idLower = idLower;
      task.startupWmClassLower = startupLower;
      task.nameLower = nameLower;
      task.appIdLower = toLower(task.appId);
      task.title = window.title;
      task.active = activeHandle != nullptr && activeHandle == window.handle;
      task.firstHandle = window.handle;
      task.iconPath = resolveIconPath(task.appId, run.entry.icon);
      task.workspaceKey = {};
      nextTasks.push_back(std::move(task));
    }
  }

  std::stable_sort(nextTasks.begin(), nextTasks.end(), [](const TaskModel& a, const TaskModel& b) {
    if (a.order != b.order) {
      return a.order < b.order;
    }
    return a.handleKey < b.handleKey;
  });

  if (compositors::isHyprland()) {
    const auto focusedCompositorWindowId = m_platform.focusedCompositorWindowId();
    for (auto& task : nextTasks) {
      if (task.firstHandle != nullptr) {
        if (const auto mappedId = m_platform.compositorWindowIdForToplevel(task.firstHandle); mappedId.has_value()) {
          task.workspaceWindowId = *mappedId;
        }
      } else if (task.workspaceWindowId.empty()) {
        const auto mappedId = m_platform.compositorWindowIdForExtToplevel(
            reinterpret_cast<ext_foreign_toplevel_handle_v1*>(task.handleKey)
        );
        if (mappedId.has_value()) {
          task.workspaceWindowId = *mappedId;
        }
      }
      if (!task.active && focusedCompositorWindowId.has_value() && !task.workspaceWindowId.empty() &&
          task.workspaceWindowId == *focusedCompositorWindowId) {
        task.active = true;
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
          if (std::find(candidate.appIds.begin(), candidate.appIds.end(), value) == candidate.appIds.end()) {
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
          if (toLower(assignment.appId) != task.appIdLower && toLower(assignment.appId) != task.idLower &&
              toLower(assignment.appId) != task.startupWmClassLower && toLower(assignment.appId) != task.nameLower) {
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
                                     ? assignment.workspaceKey + "\n" + assignment.appId + "\n" + assignment.title +
                                           "\n" + std::to_string(index)
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
        return assignmentAppLower == task.appIdLower || assignmentAppLower == task.idLower ||
               assignmentAppLower == task.startupWmClassLower || assignmentAppLower == task.nameLower;
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
          if (previous != previousWorkspaceByHandle.end() && assignment.workspaceKey != previous->second &&
              isTransientWorkspace(assignment.workspaceKey)) {
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
          if (previous != previousWorkspaceByHandle.end() && assignment.workspaceKey != previous->second &&
              isTransientWorkspace(assignment.workspaceKey)) {
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
          if (assignmentAppLower != task.appIdLower && assignmentAppLower != task.idLower &&
              assignmentAppLower != task.startupWmClassLower && assignmentAppLower != task.nameLower) {
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
          return assignmentAppLower == task.appIdLower || assignmentAppLower == task.idLower ||
                 assignmentAppLower == task.startupWmClassLower || assignmentAppLower == task.nameLower;
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
                                     ? assignment.workspaceKey + "\n" + assignment.appId + "\n" + assignment.title +
                                           "\n" + std::to_string(index)
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
      const auto* list =
          byKey != runningByWorkspace.end()
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
  const bool hasStableWorkspaceWindowAssignments = std::any_of(
      workspaceAssignments.begin(), workspaceAssignments.end(),
      [](const WorkspaceWindowAssignment& assignment) { return !assignment.windowId.empty(); }
  );
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
    activeKeys.reserve(nextWorkspaces.size());
    for (const auto& wsm : nextWorkspaces) {
      if (wsm.workspace.active) {
        activeKeys.insert(wsm.key);
      }
    }
    if (!activeKeys.empty()) {
      nextTasks.erase(
          std::remove_if(
              nextTasks.begin(), nextTasks.end(),
              [&activeKeys](const TaskModel& t) {
                return !t.workspaceKey.empty() && activeKeys.find(t.workspaceKey) == activeKeys.end();
              }
          ),
          nextTasks.end()
      );
      if (m_groupByWorkspace) {
        nextWorkspaces.erase(
            std::remove_if(
                nextWorkspaces.begin(), nextWorkspaces.end(),
                [](const WorkspaceModel& wsm) { return !wsm.workspace.active; }
            ),
            nextWorkspaces.end()
        );
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
    nextWorkspaces.erase(
        std::remove_if(
            nextWorkspaces.begin(), nextWorkspaces.end(),
            [&](const WorkspaceModel& wsm) { return !workspaceHasTask(wsm, nextTasks); }
        ),
        nextWorkspaces.end()
    );
  }

  if (!m_groupByWorkspace) {
    nextWorkspaces.clear();
    std::stable_sort(nextTasks.begin(), nextTasks.end(), [](const TaskModel& a, const TaskModel& b) {
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
  const auto& entriesIndex = desktopEntries();
  for (const auto& entry : entriesIndex) {
    if (entry.idLower == task.idLower || entry.idLower == task.appIdLower ||
        entry.startupWmClassLower == task.idLower || entry.startupWmClassLower == task.startupWmClassLower ||
        entry.nameLower == task.nameLower) {
      entryActions = entry.actions;
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
  m_contextMenuPopup->setShadowConfig(m_shadowConfig);
  m_contextMenuPopup->setOnActivate([this, entryActions](const ContextMenuControlEntry& entry) {
    if (entry.id >= 0) {
      const auto idx = static_cast<std::size_t>(entry.id);
      if (idx < entryActions.size()) {
        const auto& action = entryActions[idx];
        std::string cmd;
        cmd.reserve(action.exec.size());
        for (std::size_t i = 0; i < action.exec.size(); ++i) {
          if (action.exec[i] == '%' && i + 1 < action.exec.size()) {
            ++i;
            continue;
          }
          cmd += action.exec[i];
        }
        while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.back()))) {
          cmd.pop_back();
        }
        if (!cmd.empty()) {
          DeferredCall::callLater([cmd]() { (void)process::runAsync(cmd); });
        }
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

  const ContextMenuPopupPlacement* placement = nullptr;
  ContextMenuPopupPlacement bottomPlacement;
  if (m_barPosition == "top") {
    anchorY = absY + area.height() + static_cast<float>(gap);
    anchorH = 1.0f;
  } else if (m_barPosition == "bottom") {
    // Mirror top: gap from the task tile edge, not the pointer (tray still uses pointer-centered icons).
    anchorX = absX;
    anchorY = absY;
    anchorW = area.width();
    anchorH = 1.0f;
    bottomPlacement = ContextMenuPopupPlacement{
        .anchor = XDG_POSITIONER_ANCHOR_TOP,
        .gravity = XDG_POSITIONER_GRAVITY_TOP,
        .offsetX = 0,
        .offsetY = -gap,
        .chromeAttachment = popup_chrome::Attachment{
            .horizontal = popup_chrome::HorizontalAttachment::Center,
            .vertical = popup_chrome::VerticalAttachment::Bottom
        },
    };
    placement = &bottomPlacement;
  } else if (m_barPosition == "left") {
    anchorX = absX + area.width() + (menuWidth * 0.5f) + static_cast<float>(gap);
    anchorW = 1.0f;
  } else if (m_barPosition == "right") {
    anchorX = absX - (menuWidth * 0.5f) - static_cast<float>(gap);
    anchorW = 1.0f;
  }

  m_contextMenuPopup->open(
      std::move(entries), menuWidth, 12, static_cast<std::int32_t>(std::round(anchorX)),
      static_cast<std::int32_t>(std::round(anchorY)), static_cast<std::int32_t>(std::round(anchorW)),
      static_cast<std::int32_t>(std::round(anchorH)), layerSurface, m_output, placement
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
  if (!workspace.id.empty()) {
    return workspace.id;
  }
  if (!workspace.coordinates.empty()) {
    return std::to_string(static_cast<std::size_t>(workspace.coordinates.front()) + 1u);
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
    if (tasks[i].appId != m_tasks[i].appId || tasks[i].iconPath != m_tasks[i].iconPath ||
        tasks[i].active != m_tasks[i].active || tasks[i].firstHandle != m_tasks[i].firstHandle ||
        tasks[i].workspaceKey != m_tasks[i].workspaceKey || tasks[i].order != m_tasks[i].order ||
        tasks[i].workspaceOrder != m_tasks[i].workspaceOrder) {
      return false;
    }
  }
  for (std::size_t i = 0; i < workspaces.size(); ++i) {
    const auto& a = workspaces[i].workspace;
    const auto& b = m_workspaces[i].workspace;
    if (a.id != b.id || a.name != b.name || a.active != b.active || a.urgent != b.urgent || a.occupied != b.occupied ||
        workspaces[i].key != m_workspaces[i].key || workspaces[i].label != m_workspaces[i].label ||
        workspaces[i].hostOutput != m_workspaces[i].hostOutput) {
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
  if (appId.empty()) {
    return {};
  }

  const int iconTargetSize = static_cast<int>(std::round(48.0f * m_contentScale));

  if (!iconNameOrPath.empty()) {
    return m_iconResolver.resolve(iconNameOrPath, iconTargetSize);
  }

  if (const auto internal = internal_apps::metadataForAppId(appId); internal.has_value()) {
    return internal->iconPath;
  }

  const std::string appIdLower = toLower(appId);
  const auto it = m_appIconsByLower.find(appIdLower);
  if (it != m_appIconsByLower.end()) {
    return m_iconResolver.resolve(it->second, iconTargetSize);
  }
  return m_iconResolver.resolve(appId, iconTargetSize);
}

bool TaskbarWidget::activeWorkspaceIndex(std::size_t& index) const {
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

ColorSpec TaskbarWidget::workspaceTextColor(const Workspace& workspace) const {
  if (workspace.urgent) {
    return colorSpecFromRole(ColorRole::OnError);
  }
  return readableColorForFill(workspaceFillColor(workspace));
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
