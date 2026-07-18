#include "shell/dock/dock_items.h"

#include "config/config_service.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/dock/dock_model.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/icon_resolver.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace {

  // Instance-count badge geometry — scales with icon size.
  constexpr float kBadgeSizeRatio = 0.30f; // fraction of icon size
  constexpr float kBadgeMinSize = 16.0f;   // minimum diameter in px
  constexpr float kBadgeFontRatio = 0.72f; // font size relative to badge diameter
  constexpr float kBadgeCornerInsetX = 0.55f;
  constexpr float kBadgeCornerInsetY = 0.45f;
  constexpr float kDotSizeRatio = 0.09f;
  constexpr float kDotMinSize = 4.0f;
  constexpr float kDotGap = 3.0f;
  constexpr float kCellPad = 6.0f;
  constexpr float kLauncherGlyphSizeRatio = 0.8f;
  constexpr float kHoverZoomLerp = 0.28f;
  constexpr float kHoverZoomReferenceFrameMs = 1000.0f / 60.0f;
  // Pointer hit padding (in item pitches) — keep generous so edge icons still magnify.
  constexpr float kHoverZoomInfluence = 2.25f;
  // Scale falloff radius — tighter than hit padding so neighbors stay closer to rest size.
  constexpr float kHoverZoomFalloffInfluence = 1.5f;
  constexpr std::int32_t kHoverZoomZScale = 100;
  constexpr auto kDragHoldDelay = std::chrono::milliseconds(300);

  [[nodiscard]] float pointerMainOnRow(const DockConfig& cfg, const InputArea* area, float localX, float localY) {
    if (area == nullptr) {
      return 0.0f;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float areaMain = vertical ? area->y() : area->x();
    return areaMain + (vertical ? localY : localX);
  }

  void resetDockItemDragState(shell::dock::DockInstance& instance) {
    instance.drag.holdTimer.stop();
    instance.drag = {};
  }

  void dismissDockTooltipImpl() {
    TooltipManager::instance().onHoverChange(nullptr, static_cast<zwlr_layer_surface_v1*>(nullptr), nullptr);
  }

  [[nodiscard]] int dockIconDecodeTargetSize(const DockConfig& cfg) {
    const float baseScale = std::max(cfg.activeScale, cfg.inactiveScale);
    const float hoverScale = cfg.magnification ? std::max(1.0f, cfg.magnificationScale) : 1.0f;
    const float peakScale = std::max(1.0f, baseScale * hoverScale);
    return std::max(1, static_cast<int>(std::round(static_cast<float>(cfg.iconSize) * peakScale)));
  }

  [[nodiscard]] float launcherIconBaseY(const DockConfig& cfg, float iconSize) {
    return cfg.launcherCustomImage.empty() ? kCellPad + (iconSize - iconSize * kLauncherGlyphSizeRatio) * 0.5f
                                           : kCellPad;
  }

  void applyHoverIconVisual(Node* iconNode, DockEdge edge, float baseX, float baseY, float iconSize, float scale) {
    if (iconNode == nullptr) {
      return;
    }
    iconNode->setScale(scale);
    const float shift = scale > 1.0f ? iconSize * (1.0f - scale) * 0.5f : 0.0f;
    float x = baseX;
    float y = baseY;
    shell::dock::shiftAlongEdge(edge, x, y, shift);
    iconNode->setPosition(x, y);
  }

  void applyHoverBadgeVisual(
      Box* badge, DockEdge edge, float iconBaseX, float iconBaseY, float iconSize, float badgeSize, float iconScale
  ) {
    if (badge == nullptr) {
      return;
    }
    badge->setScale(iconScale);
    const float shift = iconScale > 1.0f ? iconSize * (1.0f - iconScale) * 0.5f : 0.0f;
    float iconX = iconBaseX;
    float iconY = iconBaseY;
    shell::dock::shiftAlongEdge(edge, iconX, iconY, shift);
    const float iconRight = iconX + iconSize * (1.0f + iconScale) * 0.5f;
    const float iconTop = iconY + iconSize * (1.0f - iconScale) * 0.5f;
    const float badgeCenterAdjust = badgeSize * (1.0f - iconScale) * 0.5f;
    badge->setPosition(
        iconRight - badgeSize * kBadgeCornerInsetX * iconScale - badgeCenterAdjust,
        iconTop - badgeSize * kBadgeCornerInsetY * iconScale - badgeCenterAdjust
    );
  }

  void applyHoverItemVisual(
      Node* iconNode, Box* badge, DockEdge edge, float iconBaseX, float iconBaseY, float iconSize, float badgeSize,
      float scale
  ) {
    applyHoverIconVisual(iconNode, edge, iconBaseX, iconBaseY, iconSize, scale);
    applyHoverBadgeVisual(badge, edge, iconBaseX, iconBaseY, iconSize, badgeSize, scale);
  }

  void applyShellAppIconColorization(Image* image, const ShellConfig& shell) {
    if (image == nullptr) {
      return;
    }
    image->setAppIconColorization(effectiveShellAppIconColorizationTint(shell));
  }

  [[nodiscard]] float itemRestCenterMain(float restMainPos, float cellMain) { return restMainPos + cellMain * 0.5f; }

  void applyItemMainOffset(InputArea* area, bool vertical, float restMain, float restCross, float offset) {
    if (area == nullptr) {
      return;
    }
    if (!vertical) {
      area->setPosition(restMain + offset, restCross);
    } else {
      area->setPosition(restCross, restMain + offset);
    }
  }

  [[nodiscard]] float hoverZoomFrameLerp(float deltaMs) {
    const float clampedMs = std::clamp(deltaMs, 1.0f, 50.0f);
    return 1.0f - std::pow(1.0f - kHoverZoomLerp, clampedMs / kHoverZoomReferenceFrameMs);
  }

  [[nodiscard]] bool lerpHoverMainOffset(float& current, float target, float lerpFactor) {
    if (std::abs(current - target) <= 0.25f) {
      if (current != target) {
        current = target;
        return true;
      }
      return false;
    }
    current += (target - current) * lerpFactor;
    return true;
  }

  void computeSymmetricSpreadOffsets(const std::vector<float>& scales, float iconSize, std::vector<float>& outOffsets) {
    const std::size_t count = scales.size();
    outOffsets.assign(count, 0.0f);
    if (count <= 1U) {
      return;
    }
    for (std::size_t index = 1; index < count; ++index) {
      const float pairExtra = iconSize * (scales[index - 1U] + scales[index] - 2.0f) * 0.5f;
      if (pairExtra <= 0.0f) {
        continue;
      }
      const float halfExtra = pairExtra * 0.5f;
      outOffsets[index - 1U] -= halfExtra;
      outOffsets[index] += halfExtra;
    }
  }

  void clampSpreadOffsetsToBounds(
      const std::vector<float>& restMainPos, const std::vector<float>& scales, float cellMain, float iconSize,
      float boundsMin, float boundsMax, std::vector<float>& offsets
  ) {
    if (offsets.empty() || restMainPos.size() != offsets.size() || scales.size() != offsets.size()) {
      return;
    }

    const auto restVisualMainMax = [&](std::size_t index) {
      return restMainPos[index] + cellMain * 0.5f + iconSize * scales[index] * 0.5f;
    };
    const auto restVisualMainMin = [&](std::size_t index) {
      return restMainPos[index] + cellMain * 0.5f - iconSize * scales[index] * 0.5f;
    };

    float spreadFactor = 1.0f;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
      const float offset = offsets[index];
      if (std::abs(offset) <= 0.001f) {
        continue;
      }
      if (offset > 0.0f) {
        const float maxRightOffset = boundsMax - restVisualMainMax(index);
        if (maxRightOffset <= 0.0f) {
          spreadFactor = 0.0f;
        } else {
          spreadFactor = std::min(spreadFactor, maxRightOffset / offset);
        }
      } else {
        const float maxLeftOffset = boundsMin - restVisualMainMin(index);
        if (maxLeftOffset >= 0.0f) {
          spreadFactor = 0.0f;
        } else {
          spreadFactor = std::min(spreadFactor, maxLeftOffset / offset);
        }
      }
    }

    spreadFactor = std::clamp(spreadFactor, 0.0f, 1.0f);
    if (spreadFactor >= 1.0f) {
      return;
    }
    for (float& offset : offsets) {
      offset *= spreadFactor;
    }
  }

  void applyHoverZoomZIndex(InputArea* area, float scale) {
    if (area == nullptr) {
      return;
    }
    area->setZIndex(static_cast<std::int32_t>(std::lround(scale * static_cast<float>(kHoverZoomZScale))));
  }

  [[nodiscard]] TooltipAnchorInsets
  scaledIconTooltipAnchor(const InputArea* area, DockEdge edge, float iconBase, float iconSize, float scale) {
    const float centerAdjust = iconSize * (1.0f - scale) * 0.5f;
    float iconX = iconBase;
    float iconY = iconBase;
    const float shift = iconSize * (1.0f - scale) * 0.5f;
    shell::dock::shiftAlongEdge(edge, iconX, iconY, shift);
    const float left = iconX + centerAdjust;
    const float top = iconY + centerAdjust;
    const float visualSize = iconSize * scale;
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0f, area->width() - left - visualSize),
        .bottom = std::max(0.0f, area->height() - top - visualSize),
        .left = left,
    };
  }

  void syncHoveredTooltipAnchor(InputArea* area, const DockConfig& cfg, float iconSize, float scale) {
    if (area == nullptr || !area->hovered()) {
      return;
    }
    area->setTooltipAnchorInsets(scaledIconTooltipAnchor(area, cfg.position, kCellPad, iconSize, scale));
    TooltipManager::instance().syncAnchor(area);
  }

  [[nodiscard]] float computeHoverMultiplier(
      float pointerMain, float itemCenterMain, float itemPitch, float maxMultiplier, float falloffInfluence
  ) {
    if (maxMultiplier <= 1.0f) {
      return 1.0f;
    }
    const float distance = std::abs(pointerMain - itemCenterMain);
    const float influence = itemPitch * falloffInfluence;
    if (distance >= influence) {
      return 1.0f;
    }
    const float t = distance / influence;
    const float cosine = std::cos(t * std::numbers::pi_v<float> * 0.5f);
    // Squared cosine keeps the center pop but drops neighbors toward rest scale faster.
    const float falloff = cosine * cosine;
    return 1.0f + (maxMultiplier - 1.0f) * falloff;
  }

  [[nodiscard]] std::string_view dockEdgeKey(DockEdge edge) {
    switch (edge) {
    case DockEdge::Top:
      return "top";
    case DockEdge::Bottom:
      return "bottom";
    case DockEdge::Left:
      return "left";
    case DockEdge::Right:
      return "right";
    }
    return "bottom";
  }

  void configureDockTooltip(InputArea& area, const DockConfig& cfg, std::string text) {
    area.setTooltip(std::move(text));
    area.setTooltipPlacement(tooltipPlacementAwayFromEdge(dockEdgeKey(cfg.position)));
    area.setTooltipAnchorInsets(
        TooltipAnchorInsets{
            .top = kCellPad,
            .right = kCellPad,
            .bottom = kCellPad,
            .left = kCellPad,
        }
    );
  }

  [[nodiscard]] std::string dockItemTooltipText(const DesktopEntry& entry) {
    if (!entry.name.empty()) {
      return entry.name;
    }
    if (!entry.genericName.empty()) {
      return entry.genericName;
    }
    return entry.id;
  }

  void applyAnimatedIconScale(
      shell::dock::DockInstance& instance, Node* iconNode, float& visualScale, AnimationManager::Id& animId,
      float targetScale
  ) {
    if (iconNode == nullptr) {
      return;
    }
    if (visualScale < 0.0f) {
      visualScale = targetScale;
      iconNode->setScale(targetScale);
      return;
    }
    if (std::abs(visualScale - targetScale) <= 0.001f) {
      return;
    }
    if (animId != 0) {
      instance.animations.cancel(animId);
    }
    animId = instance.animations.animate(
        visualScale, targetScale, Style::animNormal, Easing::EaseOutCubic,
        [node = iconNode, visualScalePtr = &visualScale](float value) {
          *visualScalePtr = value;
          node->setScale(value);
        },
        [animIdPtr = &animId] { *animIdPtr = 0; }
    );
  }

  [[nodiscard]] bool applyHoverIconScale(
      Node* iconNode, float& visualScale, AnimationManager::Id& animId, shell::dock::DockInstance& instance,
      float targetScale, float restScale, float lerpFactor, DockEdge edge, float baseX, float baseY, float iconSize,
      Box* badge, float badgeSize
  ) {
    if (iconNode == nullptr) {
      return false;
    }
    if (animId != 0) {
      instance.animations.cancel(animId);
      animId = 0;
    }
    if (visualScale < 0.0f) {
      visualScale = restScale;
      applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, restScale);
    }
    if (std::abs(visualScale - targetScale) <= 0.002f) {
      if (std::abs(visualScale - targetScale) > 0.0f) {
        visualScale = targetScale;
        applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, targetScale);
      }
      return false;
    }
    const float next = visualScale + (targetScale - visualScale) * lerpFactor;
    visualScale = next;
    applyHoverItemVisual(iconNode, badge, edge, baseX, baseY, iconSize, badgeSize, next);
    return true;
  }

} // namespace

namespace shell::dock {

  struct DockItemClickContext {
    ConfigService& config;
    DockItemCallbacks callbacks;
  };

  std::string_view dockLauncherIconGlyph(const DockConfig& cfg) {
    return cfg.launcherIcon.empty() ? "grid-dots" : std::string_view{cfg.launcherIcon};
  }

  std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical) {
    const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
    const auto crossPad = static_cast<float>(cfg.crossAxisPadding);
    return ui::flex(
        vertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
        {
            .align = FlexAlign::Center,
            .gap = static_cast<float>(cfg.itemSpacing),
            .paddingV = vertical ? mainPad : crossPad,
            .paddingH = vertical ? crossPad : mainPad,
        }
    );
  }

  void handleItemClick(DockInstance& instance, const DockItemAction& action, DockItemClickContext& context);

  std::unique_ptr<InputArea> createLauncherButton(
      DockInstance& instance, const DockConfig& cfg, RenderContext& renderContext,
      const std::shared_ptr<DockItemClickContext>& clickContext
  ) {
    const DockEdge edge = cfg.position;
    const bool vert = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const float cellMain = iSize + 2.0f * kCellPad;
    const float cellCross = iSize + 2.0f * kCellPad;
    const int iconDecodeTarget = dockIconDecodeTargetSize(cfg);

    auto areaNode = std::make_unique<InputArea>();
    if (!vert) {
      areaNode->setSize(cellMain, cellCross);
    } else {
      areaNode->setSize(cellCross, cellMain);
    }

    if (!cfg.launcherCustomImage.empty()) {
      RenderContext* renderContextPtr = &renderContext;
      auto launcherImage = ui::image({
          .fit = ImageFit::Contain,
          .width = iSize,
          .height = iSize,
          .configure = [&cfg, iSize, renderContextPtr, iconDecodeTarget](Image& image) {
            image.setSourceFile(*renderContextPtr, cfg.launcherCustomImage, iconDecodeTarget, true);
            image.setForegroundTint(
                cfg.launcherCustomImageColorize ? std::optional<ColorSpec>{colorSpecFromRole(ColorRole::OnSurface)}
                                                : std::nullopt
            );
            image.setPosition(kCellPad, launcherIconBaseY(cfg, iSize));
          },
      });
      instance.launcherIconNode = static_cast<Image*>(launcherImage.get());
      areaNode->addChild(std::move(launcherImage));
    } else {
      auto launcherGlyph = ui::glyph({
          .glyphSize = iSize * kLauncherGlyphSizeRatio,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .width = iSize,
          .height = iSize,
          .configure = [&cfg, iSize](Glyph& glyph) {
            if (!glyph.setGlyph(dockLauncherIconGlyph(cfg))) {
              glyph.setGlyph("grid-dots");
            }
            glyph.setPosition(kCellPad, launcherIconBaseY(cfg, iSize));
          },
      });
      instance.launcherIconNode = static_cast<Glyph*>(launcherGlyph.get());
      areaNode->addChild(std::move(launcherGlyph));
    }
    instance.launcherArea = areaNode.get();
    instance.launcherVisualScale = cfg.inactiveScale;
    instance.launcherIconNode->setScale(cfg.inactiveScale);
    if (instance.launcherScaleAnimId != 0) {
      instance.animations.cancel(instance.launcherScaleAnimId);
      instance.launcherScaleAnimId = 0;
    }

    auto* instPtr = &instance;
    configureDockTooltip(*areaNode, cfg, i18n::tr("dock.launcher"));
    areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
    areaNode->setOnClick([instPtr, clickContext](const InputArea::PointerData& d) {
      if (d.button == BTN_LEFT && clickContext->callbacks.toggleLauncher) {
        clickContext->callbacks.toggleLauncher(*instPtr);
      }
    });

    return areaNode;
  }

  void rebuildItems(
      DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot,
      const DockItemCallbacks& callbacks
  ) {
    uiAssertNotRendering("shell::dock::rebuildItems");
    if (instance.row == nullptr) {
      return;
    }

    resetDockItemDragState(instance);

    const auto& cfg = deps.model.config.config().dock;
    const DockEdge edge = cfg.position;
    const DockLauncherPosition launcherPosition = cfg.launcherPosition;
    const bool vert = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const int iconDecodeTarget = dockIconDecodeTargetSize(cfg);
    auto clickContext = std::make_shared<DockItemClickContext>(DockItemClickContext{
        .config = deps.model.config,
        .callbacks = callbacks,
    });

    for (auto& item : instance.items) {
      if (item.scaleAnimId != 0) {
        instance.animations.cancel(item.scaleAnimId);
        item.scaleAnimId = 0;
      }
      if (item.opacityAnimId != 0) {
        instance.animations.cancel(item.opacityAnimId);
        item.opacityAnimId = 0;
      }
    }

    // Clear previous items by recreating the row.
    if (instance.row != nullptr && instance.slideRoot != nullptr) {
      instance.slideRoot->removeChild(instance.row);
      instance.row = nullptr;
    }
    instance.items.clear();
    instance.launcherArea = nullptr;
    instance.launcherIconNode = nullptr;
    instance.launcherVisualScale = -1.0f;
    if (instance.launcherScaleAnimId != 0) {
      instance.animations.cancel(instance.launcherScaleAnimId);
      instance.launcherScaleAnimId = 0;
    }

    auto freshRow = makeDockItemRow(cfg, vert);
    Node* rowParent =
        instance.slideRoot != nullptr ? static_cast<Node*>(instance.slideRoot) : static_cast<Node*>(instance.panel);
    instance.row = static_cast<Flex*>(rowParent->addChild(std::move(freshRow)));
    const auto& itemModels = snapshot.items;

    if (launcherPosition == DockLauncherPosition::Start) {
      instance.row->addChild(createLauncherButton(instance, cfg, deps.renderContext, clickContext));
    }

    // Reserve up-front so emplace_back never reallocates while lambdas hold raw pointers.
    instance.items.reserve(itemModels.size());
    const std::size_t pinnedCount = cfg.pinned.size();

    for (std::size_t itemIndex = 0; itemIndex < itemModels.size(); ++itemIndex) {
      const auto& model = itemModels[itemIndex];
      auto& item = instance.items.emplace_back();
      DockItemAction action{
          .entry = model.entry,
          .idLower = model.idLower,
          .startupWmClassLower = model.startupWmClassLower,
          .windowLookupIdLower = model.windowLookupIdLower,
          .windowLookupWmClassLower = model.windowLookupWmClassLower,
      };

      const float cellMain = iSize + 2.0f * kCellPad;
      const float cellCross = iSize + 2.0f * kCellPad;
      auto areaNode = std::make_unique<InputArea>();
      if (!vert) {
        areaNode->setSize(cellMain, cellCross);
      } else {
        areaNode->setSize(cellCross, cellMain);
      }

      std::string iconPath;
      if (!model.entry.icon.empty()) {
        iconPath = deps.iconResolver.resolve(model.entry.icon, iconDecodeTarget);
      }
      if (iconPath.empty()) {
        if (const auto internal = internal_apps::metadataForDesktopEntry(model.entry); internal.has_value()) {
          iconPath = internal->iconPath;
        }
      }
      if (iconPath.empty()) {
        iconPath = deps.iconResolver.resolve("application-x-executable", iconDecodeTarget);
      }
      RenderContext* renderContext = &deps.renderContext;
      auto iconImg = ui::image({
          .width = iSize,
          .height = iSize,
          .configure = [renderContext, iconPath, iconDecodeTarget,
                        &shell = deps.model.config.config().shell](Image& image) {
            image.setAppIconColorization(effectiveShellAppIconColorizationTint(shell));
            if (!iconPath.empty() && renderContext != nullptr) {
              image.setSourceFile(*renderContext, iconPath, iconDecodeTarget, true);
            }
            image.setPosition(kCellPad, kCellPad);
          },
      });

      if (iconImg->hasImage()) {
        item.iconImage = static_cast<Image*>(areaNode->addChild(std::move(iconImg)));
      } else {
        item.iconGlyph = static_cast<Glyph*>(areaNode->addChild(
            ui::glyph({
                .glyph = "app-window",
                .glyphSize = iSize,
                .color = colorSpecFromRole(ColorRole::OnSurface),
                .width = iSize,
                .height = iSize,
                .configure = [](Glyph& glyph) { glyph.setPosition(kCellPad, kCellPad); },
            })
        ));
      }

      if (cfg.showDots) {
        const float dot = std::max(kDotMinSize, std::round(iSize * kDotSizeRatio));
        const bool verticalDots = shell::dock::isVerticalEdge(edge);

        for (auto& dotIndicator : item.dotIndicators) {
          dotIndicator = static_cast<Box*>(areaNode->addChild(
              ui::box({
                  .fill = colorSpecFromRole(ColorRole::Secondary),
                  .radius = dot * 0.5f,
                  .width = dot,
                  .height = dot,
                  .visible = false,
                  .configure = [verticalDots, edge, cellMain, dot](Box& box) {
                    if (verticalDots) {
                      const float x = edge == DockEdge::Left ? 1.0f : std::round(cellMain - dot - 1.0f);
                      box.setPosition(x, std::round((cellMain - dot) * 0.5f));
                    } else {
                      const float y = edge == DockEdge::Bottom ? std::round(cellMain - dot - 1.0f) : 1.0f;
                      box.setPosition(std::round((cellMain - dot) * 0.5f), y);
                    }
                  },
              })
          ));
        }
      }

      if (cfg.showInstanceCount) {
        const float bd = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
        const float badgeX = kCellPad + iSize - bd * kBadgeCornerInsetX;
        const float badgeY = kCellPad - bd * kBadgeCornerInsetY;

        areaNode->addChild(
            ui::box({
                .out = &item.badge,
                .radius = bd * 0.5f,
                .width = bd,
                .height = bd,
                .visible = false,
                .configure = [badgeX, badgeY](Box& box) { box.setPosition(badgeX, badgeY); },
            })
        );

        item.badge->addChild(
            ui::label({
                .out = &item.badgeLabel,
                .fontSize = bd * kBadgeFontRatio,
                .fontWeight = FontWeight::Bold,
                .maxLines = 1,
                .visible = false,
            })
        );
      }

      auto* instPtr = &instance;

      configureDockTooltip(*areaNode, cfg, dockItemTooltipText(model.entry));
      areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
      const bool itemPinned = itemIndex < pinnedCount;
      areaNode->setOnPress([instPtr, clickContext, itemIndex, itemPinned](const InputArea::PointerData& d) {
        if (d.button != BTN_LEFT) {
          return;
        }
        auto& drag = instPtr->drag;
        const auto& dockCfg = clickContext->config.config().dock;
        if (d.pressed) {
          if (!itemPinned) {
            return;
          }
          resetDockItemDragState(*instPtr);
          drag.pinned = true;
          drag.sourceIndex = itemIndex;
          drag.startMain = pointerMainOnRow(dockCfg, instPtr->items[itemIndex].area, d.localX, d.localY);
          drag.currentMain = drag.startMain;
          drag.targetIndex = itemIndex;
          drag.holdTimer.start(kDragHoldDelay, [instPtr, itemIndex]() {
            if (instPtr->drag.sourceIndex == itemIndex && !instPtr->drag.active) {
              instPtr->drag.armed = true;
              dismissDockTooltipImpl();
            }
          });
          return;
        }

        if (drag.sourceIndex != itemIndex) {
          return;
        }

        if (drag.active) {
          if (clickContext->callbacks.endDrag) {
            clickContext->callbacks.endDrag(*instPtr, true);
          }
          return;
        }

        drag.holdTimer.stop();
        if (drag.armed) {
          if (clickContext->callbacks.endDrag) {
            clickContext->callbacks.endDrag(*instPtr, false);
          }
        } else {
          resetDockItemDragState(*instPtr);
        }
      });
      areaNode->setOnMotion([instPtr, clickContext, itemIndex, itemPinned](const InputArea::PointerData& d) {
        if (!itemPinned || instPtr->drag.sourceIndex != itemIndex) {
          return;
        }
        if (!instPtr->drag.armed && !instPtr->drag.active) {
          return;
        }

        const auto& dockCfg = clickContext->config.config().dock;
        const float mainPos = pointerMainOnRow(dockCfg, instPtr->items[itemIndex].area, d.localX, d.localY);
        instPtr->drag.currentMain = mainPos;
        if (instPtr->drag.active) {
          if (clickContext->callbacks.updateDrag) {
            clickContext->callbacks.updateDrag(*instPtr, mainPos);
          }
        } else if (instPtr->drag.armed && clickContext->callbacks.beginDrag) {
          clickContext->callbacks.beginDrag(*instPtr, itemIndex, mainPos);
        }
      });
      areaNode->setOnClick([action = std::move(action), instPtr, clickContext](const InputArea::PointerData& d) {
        if (d.button == BTN_RIGHT) {
          if (clickContext->callbacks.openItemMenu) {
            clickContext->callbacks.openItemMenu(*instPtr, action);
          }
          return;
        }
        if (d.button != BTN_LEFT) {
          return;
        }
        if (instPtr->suppressItemClick) {
          instPtr->suppressItemClick = false;
          return;
        }
        if (instPtr->drag.active || instPtr->drag.armed) {
          return;
        }
        handleItemClick(*instPtr, action, *clickContext);
      });

      item.area = static_cast<InputArea*>(instance.row->addChild(std::move(areaNode)));
      const float iconScale = model.active ? cfg.activeScale : cfg.inactiveScale;
      if (cfg.magnification) {
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          item.visualScale = iconScale;
          item.hoverMainOffset = 0.0f;
          const float badgeSize = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
          applyHoverItemVisual(iconNode, item.badge, edge, kCellPad, kCellPad, iSize, badgeSize, iconScale);
        }
      }
    }

    if (launcherPosition == DockLauncherPosition::End) {
      instance.row->addChild(createLauncherButton(instance, cfg, deps.renderContext, clickContext));
    }

    if (cfg.magnification && instance.launcherIconNode != nullptr) {
      const float launcherScale = cfg.inactiveScale;
      instance.launcherVisualScale = launcherScale;
      applyHoverItemVisual(
          instance.launcherIconNode, nullptr, edge, kCellPad, launcherIconBaseY(cfg, iSize), iSize, 0.0f, launcherScale
      );
    }

    shell::dock::resizeSurface(instance, cfg, deps.model.config.config().shell.shadow);

    if (cfg.magnification && instance.surface != nullptr) {
      instance.surface->requestFrameTick();
      instance.surface->requestRedraw();
    }
  }

  void updateVisuals(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot) {
    const auto& cfg = deps.model.config.config().dock;
    const auto& shell = deps.model.config.config().shell;
    const DockEdge edge = cfg.position;
    const std::size_t itemCount = std::min(instance.items.size(), snapshot.items.size());

    for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      auto& item = instance.items[itemIndex];
      const auto& model = snapshot.items[itemIndex];
      const bool dragActive = instance.drag.active;
      const bool isDraggedItem = dragActive && itemIndex == instance.drag.sourceIndex;
      const float iconScale = model.active ? cfg.activeScale : cfg.inactiveScale;
      const float iconOpacity = model.active ? cfg.activeOpacity : cfg.inactiveOpacity;
      applyShellAppIconColorization(item.iconImage, shell);
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);

      if (iconNode != nullptr) {
        if (!cfg.magnification) {
          if (!isDraggedItem) {
            iconNode->setPosition(kCellPad, kCellPad);
          }
          if (item.visualScale < 0.0f) {
            item.visualScale = iconScale;
            iconNode->setScale(iconScale);
          } else if (!isDraggedItem && std::abs(item.visualScale - iconScale) > 0.001f) {
            applyAnimatedIconScale(instance, iconNode, item.visualScale, item.scaleAnimId, iconScale);
          }
        }

        if (item.badge != nullptr && !cfg.magnification) {
          const float bd = std::max(kBadgeMinSize, static_cast<float>(cfg.iconSize) * kBadgeSizeRatio);
          item.badge->setScale(1.0f);
          item.badge->setPosition(
              kCellPad + static_cast<float>(cfg.iconSize) - bd * kBadgeCornerInsetX, kCellPad - bd * kBadgeCornerInsetY
          );
        }

        if (!cfg.magnification && !dragActive) {
          item.hoverMainOffset = 0.0f;
          applyItemMainOffset(item.area, shell::dock::isVerticalEdge(edge), item.restMainPos, item.restCrossPos, 0.0f);
          if (item.area != nullptr) {
            item.area->setZIndex(0);
          }
        }

        if (!isDraggedItem) {
          if (item.visualOpacity < 0.0f) {
            item.visualOpacity = iconOpacity;
            iconNode->setOpacity(iconOpacity);
          } else if (std::abs(item.visualOpacity - iconOpacity) > 0.001f) {
            if (item.opacityAnimId != 0) {
              instance.animations.cancel(item.opacityAnimId);
            }
            item.opacityAnimId = instance.animations.animate(
                item.visualOpacity, iconOpacity, Style::animNormal, Easing::EaseOutCubic,
                [node = iconNode, itemPtr = &item](float value) {
                  itemPtr->visualOpacity = value;
                  node->setOpacity(value);
                },
                [itemPtr = &item] { itemPtr->opacityAnimId = 0; }
            );
          }
        }
      }

      const std::size_t count = model.instanceCount;

      if (cfg.showDots) {
        const std::size_t dotCount = std::min<std::size_t>(count, 3);
        const auto iSize = static_cast<float>(cfg.iconSize);
        const float cellMain = iSize + 2.0f * kCellPad;
        const float dot = std::max(kDotMinSize, std::round(iSize * kDotSizeRatio));
        const float groupLength =
            dotCount == 0 ? dot : dot * static_cast<float>(dotCount) + kDotGap * static_cast<float>(dotCount - 1);
        const float groupStart = std::round((cellMain - groupLength) * 0.5f);
        const bool verticalDots = shell::dock::isVerticalEdge(edge);

        for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
          if (item.dotIndicators[dotIndex] == nullptr) {
            continue;
          }
          Box* dotNode = item.dotIndicators[dotIndex];
          const bool visible = dotIndex < dotCount;
          dotNode->setVisible(visible);
          dotNode->setFill(colorSpecFromRole(ColorRole::Secondary));
          if (visible) {
            const float main = groupStart + static_cast<float>(dotIndex) * (dot + kDotGap);
            if (verticalDots) {
              const float x = edge == DockEdge::Left ? 1.0f : std::round(cellMain - dot - 1.0f);
              dotNode->setPosition(x, main);
            } else {
              const float y = edge == DockEdge::Bottom ? std::round(cellMain - dot - 1.0f) : 1.0f;
              dotNode->setPosition(main, y);
            }
          }
        }
      }

      if (item.badge != nullptr && item.badgeLabel != nullptr) {
        const bool show = count >= 2;
        item.badge->setVisible(show);
        item.badgeLabel->setVisible(show);
        if (show) {
          const std::string label = (count > 9) ? "9+" : std::to_string(count);
          item.badgeLabel->setText(label);
          item.badgeLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
          item.badge->setFill(colorSpecFromRole(ColorRole::Primary));
          const float bd = std::max(kBadgeMinSize, static_cast<float>(cfg.iconSize) * kBadgeSizeRatio);
          item.badgeLabel->measure(deps.renderContext);
          item.badgeLabel->setPosition(
              std::round((bd - item.badgeLabel->width()) * 0.5f), std::round((bd - item.badgeLabel->height()) * 0.5f)
          );
        }
      }
    }

    if (instance.drag.active) {
      applyDragVisuals(instance, cfg);
    }

    if (!cfg.magnification && instance.launcherIconNode != nullptr) {
      const float iconScale = cfg.inactiveScale;
      const auto iSize = static_cast<float>(cfg.iconSize);
      instance.launcherIconNode->setPosition(kCellPad, launcherIconBaseY(cfg, iSize));
      if (instance.launcherVisualScale < 0.0f) {
        instance.launcherVisualScale = iconScale;
        instance.launcherIconNode->setScale(iconScale);
      } else if (std::abs(instance.launcherVisualScale - iconScale) > 0.001f) {
        applyAnimatedIconScale(
            instance, instance.launcherIconNode, instance.launcherVisualScale, instance.launcherScaleAnimId, iconScale
        );
      }
      instance.launcherHoverMainOffset = 0.0f;
      applyItemMainOffset(
          instance.launcherArea, shell::dock::isVerticalEdge(edge), instance.launcherRestMainPos,
          instance.launcherRestCrossPos, 0.0f
      );
      if (instance.launcherArea != nullptr) {
        instance.launcherArea->setZIndex(0);
      }
    }

    // Magnification owns active/inactive scale via updateHoverZoom; kick it on focus updates.
    if (cfg.magnification && instance.surface != nullptr) {
      if (updateHoverZoom(instance, deps, snapshot, kHoverZoomReferenceFrameMs)) {
        instance.surface->requestFrameTick();
        instance.surface->requestRedraw();
      }
    }
  }

  void clearHoverZoom(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot) {
    instance.hoverPointerValid = false;
    if (instance.surface == nullptr) {
      return;
    }
    (void)updateHoverZoom(instance, deps, snapshot, kHoverZoomReferenceFrameMs);
    instance.surface->requestFrameTick();
    instance.surface->requestRedraw();
  }

  bool syncHoverPointerFromScene(DockInstance& instance, const DockConfig& cfg, float sceneX, float sceneY) {
    if (instance.row == nullptr) {
      instance.hoverPointerValid = false;
      return false;
    }

    float localX = 0.0f;
    float localY = 0.0f;
    if (!Node::mapFromScene(instance.row, sceneX, sceneY, localX, localY)) {
      instance.hoverPointerValid = false;
      return false;
    }

    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0f * kCellPad;
    const float itemPitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const float mainPad = itemPitch * kHoverZoomInfluence;
    const float crossPad = static_cast<float>(shell::dock::dockHoverZoomCrossPad(cfg)) + kCellPad;
    const float main = vertical ? localY : localX;
    const float cross = vertical ? localX : localY;
    const float mainSize = vertical ? instance.row->height() : instance.row->width();
    const float crossSize = vertical ? instance.row->width() : instance.row->height();
    if (main < -mainPad || main > mainSize + mainPad || cross < -crossPad || cross > crossSize + crossPad) {
      instance.hoverPointerValid = false;
      return false;
    }

    instance.hoverPointerMain = main;
    instance.hoverPointerValid = true;
    return true;
  }

  bool
  updateHoverZoom(DockInstance& instance, DockItemSceneDependencies deps, const DockSnapshot& snapshot, float deltaMs) {
    const auto& cfg = deps.model.config.config().dock;
    if (!cfg.magnification || instance.row == nullptr) {
      return false;
    }
    if (instance.drag.active) {
      applyDragVisuals(instance, cfg);
      return false;
    }

    const float lerpFactor = hoverZoomFrameLerp(deltaMs);
    const DockEdge edge = cfg.position;
    const bool vertical = shell::dock::isVerticalEdge(edge);
    const auto iSize = static_cast<float>(cfg.iconSize);
    const float cellMain = iSize + 2.0f * kCellPad;
    const float itemPitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const float badgeSize = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
    const float baseLauncherScale = cfg.inactiveScale;
    const bool pointerActive = instance.pointerInside && instance.hoverPointerValid;
    const std::size_t itemCount = std::min(instance.items.size(), snapshot.items.size());
    bool needsMoreFrames = false;

    struct HoverSlot {
      InputArea* area = nullptr;
      Box* badge = nullptr;
      Node* iconNode = nullptr;
      float* visualScale = nullptr;
      float* hoverMainOffset = nullptr;
      AnimationManager::Id* scaleAnimId = nullptr;
      float restMainPos = 0.0f;
      float restCrossPos = 0.0f;
      float baseScale = 1.0f;
      float targetScale = 1.0f;
      float targetMainOffset = 0.0f;
      float restCenterMain = 0.0f;
      float iconBaseX = kCellPad;
      float iconBaseY = kCellPad;
    };

    std::vector<HoverSlot> slots;
    slots.reserve(itemCount + 1U);
    if (cfg.launcherPosition == DockLauncherPosition::Start && instance.launcherArea != nullptr) {
      slots.push_back(
          HoverSlot{
              .area = instance.launcherArea,
              .iconNode = instance.launcherIconNode,
              .visualScale = &instance.launcherVisualScale,
              .hoverMainOffset = &instance.launcherHoverMainOffset,
              .scaleAnimId = &instance.launcherScaleAnimId,
              .restMainPos = instance.launcherRestMainPos,
              .restCrossPos = instance.launcherRestCrossPos,
              .baseScale = baseLauncherScale,
              .restCenterMain = itemRestCenterMain(instance.launcherRestMainPos, cellMain),
              .iconBaseX = kCellPad,
              .iconBaseY = launcherIconBaseY(cfg, iSize),
          }
      );
    }
    for (std::size_t itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
      auto& item = instance.items[itemIndex];
      const auto& model = snapshot.items[itemIndex];
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
      slots.push_back(
          HoverSlot{
              .area = item.area,
              .badge = item.badge,
              .iconNode = iconNode,
              .visualScale = &item.visualScale,
              .hoverMainOffset = &item.hoverMainOffset,
              .scaleAnimId = &item.scaleAnimId,
              .restMainPos = item.restMainPos,
              .restCrossPos = item.restCrossPos,
              .baseScale = model.active ? cfg.activeScale : cfg.inactiveScale,
              .restCenterMain = itemRestCenterMain(item.restMainPos, cellMain),
          }
      );
    }
    if (cfg.launcherPosition == DockLauncherPosition::End && instance.launcherArea != nullptr) {
      slots.push_back(
          HoverSlot{
              .area = instance.launcherArea,
              .iconNode = instance.launcherIconNode,
              .visualScale = &instance.launcherVisualScale,
              .hoverMainOffset = &instance.launcherHoverMainOffset,
              .scaleAnimId = &instance.launcherScaleAnimId,
              .restMainPos = instance.launcherRestMainPos,
              .restCrossPos = instance.launcherRestCrossPos,
              .baseScale = baseLauncherScale,
              .restCenterMain = itemRestCenterMain(instance.launcherRestMainPos, cellMain),
              .iconBaseX = kCellPad,
              .iconBaseY = launcherIconBaseY(cfg, iSize),
          }
      );
    }

    std::vector<float> slotScales;
    slotScales.reserve(slots.size());
    for (HoverSlot& slot : slots) {
      const float hoverMultiplier = pointerActive ? computeHoverMultiplier(
                                                        instance.hoverPointerMain, slot.restCenterMain, itemPitch,
                                                        cfg.magnificationScale, kHoverZoomFalloffInfluence
                                                    )
                                                  : 1.0f;
      slot.targetScale = slot.baseScale * hoverMultiplier;
      slotScales.push_back(slot.targetScale);
    }

    std::vector<float> targetOffsets;
    if (pointerActive) {
      computeSymmetricSpreadOffsets(slotScales, iSize, targetOffsets);
      std::vector<float> restMainPos;
      restMainPos.reserve(slots.size());
      for (const HoverSlot& slot : slots) {
        restMainPos.push_back(slot.restMainPos);
      }
      const float rowMainSize = vertical ? instance.row->height() : instance.row->width();
      const auto mainPad = static_cast<float>(cfg.mainAxisPadding);
      clampSpreadOffsetsToBounds(
          restMainPos, slotScales, cellMain, iSize, mainPad, rowMainSize - mainPad, targetOffsets
      );
    } else {
      targetOffsets.assign(slots.size(), 0.0f);
    }

    for (std::size_t index = 0; index < slots.size(); ++index) {
      slots[index].targetMainOffset = targetOffsets[index];
    }

    for (HoverSlot& slot : slots) {
      if (lerpHoverMainOffset(*slot.hoverMainOffset, pointerActive ? slot.targetMainOffset : 0.0f, lerpFactor)) {
        needsMoreFrames = true;
      }
      applyItemMainOffset(slot.area, vertical, slot.restMainPos, slot.restCrossPos, *slot.hoverMainOffset);

      if (applyHoverIconScale(
              slot.iconNode, *slot.visualScale, *slot.scaleAnimId, instance, slot.targetScale, slot.baseScale,
              lerpFactor, edge, slot.iconBaseX, slot.iconBaseY, iSize, slot.badge, badgeSize
          )) {
        needsMoreFrames = true;
      }

      const float paintScale = *slot.visualScale > 0.0f ? *slot.visualScale : slot.targetScale;
      applyHoverZoomZIndex(slot.area, paintScale);
      if (!instance.drag.active) {
        syncHoveredTooltipAnchor(slot.area, cfg, iSize, paintScale);
      }
    }

    if (needsMoreFrames && instance.sceneRoot != nullptr) {
      instance.sceneRoot->markPaintDirty();
    }
    return needsMoreFrames;
  }

  void syncDockItemRestPositions(DockInstance& instance, const DockConfig& cfg) {
    if (instance.row == nullptr) {
      return;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    if (cfg.launcherPosition == DockLauncherPosition::Start && instance.launcherArea != nullptr) {
      instance.launcherRestMainPos = vertical ? instance.launcherArea->y() : instance.launcherArea->x();
      instance.launcherRestCrossPos = vertical ? instance.launcherArea->x() : instance.launcherArea->y();
      instance.launcherHoverMainOffset = 0.0f;
    }
    for (auto& item : instance.items) {
      if (item.area == nullptr) {
        continue;
      }
      item.restMainPos = vertical ? item.area->y() : item.area->x();
      item.restCrossPos = vertical ? item.area->x() : item.area->y();
      item.hoverMainOffset = 0.0f;
    }
    if (cfg.launcherPosition == DockLauncherPosition::End && instance.launcherArea != nullptr) {
      instance.launcherRestMainPos = vertical ? instance.launcherArea->y() : instance.launcherArea->x();
      instance.launcherRestCrossPos = vertical ? instance.launcherArea->x() : instance.launcherArea->y();
      instance.launcherHoverMainOffset = 0.0f;
    }
  }

  void handleItemClick(DockInstance& instance, const DockItemAction& action, DockItemClickContext& context) {
    if (context.callbacks.activateOrLaunch) {
      context.callbacks.activateOrLaunch(instance, action);
    }
  }

  // ── Drag-to-reorder ──────────────────────────────────────────────────────────

  std::size_t computeDragTargetIndex(const DockInstance& instance, const DockConfig& cfg, float mainPos) {
    const std::size_t pinnedCount = cfg.pinned.size();
    if (pinnedCount == 0 || instance.items.empty()) {
      return 0;
    }
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0f * kCellPad;
    const float pitch = cellMain + static_cast<float>(cfg.itemSpacing);

    std::size_t target = 0;
    const std::size_t limit = std::min(pinnedCount, instance.items.size());
    for (std::size_t i = 0; i < limit; ++i) {
      const auto& item = instance.items[i];
      if (item.area == nullptr) {
        continue;
      }
      const float restCenter = item.restMainPos + cellMain * 0.5f;
      if (mainPos > restCenter + pitch * 0.05f) {
        target = i + 1;
      }
    }
    return std::min(target, pinnedCount);
  }

  void applyDragVisuals(DockInstance& instance, const DockConfig& cfg) {
    if (!instance.drag.active || instance.items.empty()) {
      return;
    }
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    const float cellMain = static_cast<float>(cfg.iconSize) + 2.0f * kCellPad;
    const float pitch = cellMain + static_cast<float>(cfg.itemSpacing);
    const std::size_t pinnedCount = cfg.pinned.size();
    const std::size_t source = instance.drag.sourceIndex;
    const std::size_t target = instance.drag.targetIndex;

    for (std::size_t i = 0; i < instance.items.size(); ++i) {
      auto& item = instance.items[i];
      if (item.area == nullptr) {
        continue;
      }

      float extraOffset = 0.0f;
      if (i == source) {
        extraOffset = instance.drag.currentMain - instance.drag.startMain;
        item.area->setZIndex(200);
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(0.85f);
        }
      } else if (i < pinnedCount) {
        item.area->setZIndex(0);
        const bool before = (i < source && i >= target);
        const bool after = (i > source && i < target);
        if (before) {
          extraOffset = pitch;
        } else if (after) {
          extraOffset = -pitch;
        }
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(item.visualOpacity >= 0.0f ? item.visualOpacity : 1.0f);
        }
      } else {
        item.area->setZIndex(0);
        Node* iconNode =
            item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
        if (iconNode != nullptr) {
          iconNode->setOpacity(item.visualOpacity >= 0.0f ? item.visualOpacity : 1.0f);
        }
      }

      item.dragMainOffset = extraOffset;
      const float totalMain = item.restMainPos + item.hoverMainOffset + extraOffset;
      if (!vertical) {
        item.area->setPosition(totalMain, item.restCrossPos);
      } else {
        item.area->setPosition(item.restCrossPos, totalMain);
      }
    }
  }

  void clearDragVisuals(DockInstance& instance, const DockConfig& cfg) {
    const bool vertical = shell::dock::isVerticalEdge(cfg.position);
    for (auto& item : instance.items) {
      if (item.area == nullptr) {
        continue;
      }
      item.dragMainOffset = 0.0f;
      item.isDragGhost = false;
      item.area->setZIndex(0);
      const float totalMain = item.restMainPos + item.hoverMainOffset;
      if (!vertical) {
        item.area->setPosition(totalMain, item.restCrossPos);
      } else {
        item.area->setPosition(item.restCrossPos, totalMain);
      }
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);
      if (iconNode != nullptr) {
        const float opacity = item.visualOpacity >= 0.0f ? item.visualOpacity : 1.0f;
        iconNode->setOpacity(opacity);
      }
    }
  }

  void dismissDockTooltip() { dismissDockTooltipImpl(); }

} // namespace shell::dock
