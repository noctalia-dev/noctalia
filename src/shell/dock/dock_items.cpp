#include "shell/dock/dock_items.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "system/app_identity.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_toplevels.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr Logger kLog("dock");

  // Instance-count badge geometry — scales with icon size.
  constexpr float kBadgeSizeRatio = 0.30f; // fraction of icon size
  constexpr float kBadgeMinSize = 16.0f;   // minimum diameter in px
  constexpr float kBadgeFontRatio = 0.72f; // font size relative to badge diameter
  constexpr float kDotSizeRatio = 0.09f;
  constexpr float kDotMinSize = 4.0f;
  constexpr float kDotGap = 3.0f;
  constexpr float kCellPad = 6.0f;

  zwlr_foreign_toplevel_handle_v1* nextActivatableWindowHandle(
      const std::vector<ToplevelInfo>& windows, zwlr_foreign_toplevel_handle_v1* activeHandle,
      zwlr_foreign_toplevel_handle_v1* preferredHandle
  ) {
    for (std::size_t i = 0; i < windows.size(); ++i) {
      if (windows[i].handle != nullptr && windows[i].handle == activeHandle) {
        for (std::size_t offset = 1; offset <= windows.size(); ++offset) {
          auto* nextHandle = windows[(i + offset) % windows.size()].handle;
          if (nextHandle != nullptr) {
            return nextHandle;
          }
        }
        return nullptr;
      }
    }

    for (const auto& window : windows) {
      if (window.handle != nullptr && window.handle == preferredHandle) {
        return window.handle;
      }
    }

    for (const auto& window : windows) {
      if (window.handle != nullptr) {
        return window.handle;
      }
    }
    return nullptr;
  }

} // namespace

namespace shell::dock {

  struct DockItemClickContext {
    CompositorPlatform& platform;
    ConfigService& config;
    std::unordered_map<std::string, zwlr_foreign_toplevel_handle_v1*>& lastActiveHandleByAppIdLower;
    DockItemCallbacks callbacks;
  };

  std::string currentActiveEntryIdLower(const CompositorPlatform& platform) {
    if (const auto active = platform.activeToplevel(); active.has_value()) {
      return StringUtils::toLower(app_identity::resolveRunningDesktopEntry(active->appId, desktopEntries()).id);
    }
    return {};
  }

  wl_output* dockFilterOutput(const DockConfig& cfg, wl_output* instanceOutput) {
    if (!cfg.activeMonitorOnly) {
      return nullptr;
    }
    return instanceOutput;
  }

  bool refreshPinnedAppsIfNeeded(
      const DockConfig& cfg, std::vector<std::string>& lastPinnedConfig, std::vector<DesktopEntry>& pinnedEntries,
      std::uint64_t& modelSerial, std::uint64_t& entriesVersion
  ) {
    if (desktopEntriesVersion() == entriesVersion && cfg.pinned == lastPinnedConfig) {
      return false;
    }

    lastPinnedConfig = cfg.pinned;
    entriesVersion = desktopEntriesVersion();
    pinnedEntries.clear();

    const auto& entries = desktopEntries();

    for (const auto& pinnedId : cfg.pinned) {
      const auto pinnedLower = StringUtils::toLower(pinnedId);
      bool found = false;

      for (const auto& entry : entries) {
        if (entry.hidden || entry.noDisplay) {
          continue;
        }
        // Match by entry ID (stem of the desktop file path, e.g. "firefox"),
        // by StartupWMClass (lower), or by Name (lower).
        const auto stemLower = StringUtils::toLower([&] {
          const auto slash = entry.id.rfind('/');
          const auto base = (slash == std::string::npos) ? entry.id : entry.id.substr(slash + 1);
          const auto dot = base.rfind('.');
          return (dot == std::string::npos) ? base : base.substr(0, dot);
        }());

        if (stemLower == pinnedLower || app_identity::desktopEntryMatchesLower(entry, pinnedLower)) {
          pinnedEntries.push_back(entry);
          found = true;
          break;
        }
      }

      if (!found) {
        kLog.debug("pinned app not found: {}", pinnedId);
        // Add placeholder so the pinned slot is visible even when app is not installed.
        DesktopEntry placeholder;
        placeholder.id = pinnedId;
        placeholder.name = pinnedId;
        placeholder.nameLower = pinnedLower;
        pinnedEntries.push_back(std::move(placeholder));
      }
    }

    ++modelSerial;
    kLog.debug("pinned app list: {} entries", pinnedEntries.size());
    return true;
  }

  bool matchesActiveApp(const DockItemView& item, std::string_view activeAppIdLower) {
    return !activeAppIdLower.empty() && activeAppIdLower == item.idLower;
  }

  bool matchesRunningApp(const DockItemView& item, const std::vector<std::string>& runningLower) {
    for (const auto& rid : runningLower) {
      if (!rid.empty() && rid == item.idLower) {
        return true;
      }
    }
    return false;
  }

  bool syncInstanceModel(DockInstance& instance, DockItemModelDependencies deps) {
    const auto& cfg = deps.config.config().dock;
    const std::string globalActiveIdLower = currentActiveEntryIdLower(deps.platform);
    if (!globalActiveIdLower.empty()) {
      if (const auto active = deps.platform.activeToplevel(); active.has_value() && active->handle != nullptr) {
        deps.lastActiveHandleByAppIdLower[globalActiveIdLower] = active->handle;
      }
    }
    wl_output* const activeOutput = deps.platform.activeToplevelOutput();
    wl_output* filterOutput = dockFilterOutput(cfg, instance.output);
    const bool filterOutputChanged = (filterOutput != instance.lastFilterOutput);
    instance.lastFilterOutput = filterOutput;

    // When filtering by active monitor, inactive monitors' docks should not
    // highlight the globally-active app — it isn't on them.
    const std::string activeIdLower =
        (cfg.activeMonitorOnly && activeOutput != instance.output) ? std::string{} : globalActiveIdLower;
    instance.activeAppIdLower = activeIdLower;

    const auto runningIds = cfg.showRunning ? deps.platform.runningAppIds(filterOutput) : std::vector<std::string>{};
    const auto& allEntries = desktopEntries();
    const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, allEntries);
    std::vector<std::string> runningLower;
    runningLower.reserve(resolvedRunning.size());
    for (const auto& run : resolvedRunning) {
      runningLower.push_back(StringUtils::toLower(run.entry.id));
    }

    bool needRebuild = (instance.modelSerial != deps.modelSerial) || filterOutputChanged;
    if (!needRebuild && cfg.showRunning) {
      const std::size_t expectedTotal = [&] {
        std::vector<DesktopEntry> entries = deps.pinnedEntries;
        for (const auto& run : resolvedRunning) {
          bool present = false;
          for (const auto& entry : entries) {
            if (app_identity::desktopEntryMatchesLower(entry, run.runningLower)) {
              present = true;
              break;
            }
          }
          if (!present) {
            entries.push_back(run.entry);
          }
        }
        return entries.size();
      }();
      if (expectedTotal != instance.items.size()) {
        needRebuild = true;
      }
    }

    for (auto& item : instance.items) {
      item.running = matchesRunningApp(item, runningLower);
      item.active = matchesActiveApp(item, activeIdLower);
    }

    return needRebuild;
  }

  std::string_view dockLauncherIconGlyph(const DockConfig& cfg) {
    return cfg.launcherIcon.empty() ? "grid-dots" : std::string_view{cfg.launcherIcon};
  }

  std::unique_ptr<Flex> makeDockItemRow(const DockConfig& cfg, bool vertical) {
    return ui::flex(
        vertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
        {
            .align = FlexAlign::Center,
            .gap = static_cast<float>(cfg.itemSpacing),
            .padding = static_cast<float>(cfg.padding),
        }
    );
  }

  void handleItemClick(DockInstance& instance, DockItemView& item, DockItemClickContext& context);

  std::unique_ptr<InputArea> createLauncherButton(
      DockInstance& instance, const DockConfig& cfg, const std::shared_ptr<DockItemClickContext>& clickContext
  ) {
    const bool vert = shell::dock::isVerticalPosition(cfg.position);
    const float iSize = static_cast<float>(cfg.iconSize);
    const float cellMain = iSize + (2.0f * kCellPad);
    const float cellCross = iSize + (2.0f * kCellPad);
    const float glyphSize = iSize * 0.8f;
    const float glyphOffsetY = kCellPad + ((iSize - glyphSize) * 0.5f);

    auto areaNode = std::make_unique<InputArea>();
    if (!vert) {
      areaNode->setSize(cellMain, cellCross);
    } else {
      areaNode->setSize(cellCross, cellMain);
    }

    Box* bgPtr = nullptr;
    areaNode->addChild(
        ui::box({
            .out = &bgPtr,
            .fill = clearColorSpec(),
            .radius = static_cast<float>(cfg.radius),
            .width = cellMain,
            .height = cellMain,
            .configure = [](Box& box) { box.setPosition(0.0f, 0.0f); },
        })
    );

    areaNode->addChild(
        ui::glyph({
            .glyphSize = glyphSize,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .width = iSize,
            .height = iSize,
            .configure = [&cfg, glyphOffsetY](Glyph& glyph) {
              if (!glyph.setGlyph(dockLauncherIconGlyph(cfg))) {
                glyph.setGlyph("grid-dots");
              }
              glyph.setPosition(kCellPad, glyphOffsetY);
            },
        })
    );

    auto* instPtr = &instance;
    areaNode->setOnEnter([bgPtr, instPtr](const InputArea::PointerData&) {
      bgPtr->setFill(colorSpecFromRole(ColorRole::Hover));
      if (instPtr->sceneRoot != nullptr) {
        instPtr->sceneRoot->markPaintDirty();
      }
    });
    areaNode->setOnLeave([bgPtr, instPtr]() {
      bgPtr->setFill(clearColorSpec());
      if (instPtr->sceneRoot != nullptr) {
        instPtr->sceneRoot->markPaintDirty();
      }
    });
    areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT}));
    areaNode->setOnClick([instPtr, clickContext](const InputArea::PointerData& d) {
      if (d.button == BTN_LEFT && clickContext->callbacks.toggleLauncher) {
        clickContext->callbacks.toggleLauncher(*instPtr);
      }
    });

    return areaNode;
  }

  void rebuildItems(DockInstance& instance, DockItemSceneDependencies deps, const DockItemCallbacks& callbacks) {
    uiAssertNotRendering("shell::dock::rebuildItems");
    if (instance.row == nullptr) {
      return;
    }

    const auto& cfg = deps.model.config.config().dock;
    const bool vert = shell::dock::isVerticalPosition(cfg.position);
    const float iSize = static_cast<float>(cfg.iconSize);
    auto clickContext = std::make_shared<DockItemClickContext>(DockItemClickContext{
        .platform = deps.model.platform,
        .config = deps.model.config,
        .lastActiveHandleByAppIdLower = deps.model.lastActiveHandleByAppIdLower,
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
    if (instance.row != nullptr && instance.panel != nullptr) {
      instance.panel->removeChild(instance.row);
      instance.row = nullptr;
    }
    instance.items.clear();

    auto freshRow = makeDockItemRow(cfg, vert);
    instance.row = dynamic_cast<Flex*>(
        instance.panel != nullptr ? instance.panel->addChild(std::move(freshRow))
                                  : instance.sceneRoot->addChild(std::move(freshRow))
    );

    // Determine items: pinned + (optionally) running-only apps not in pinned.
    std::vector<DesktopEntry> itemEntries = deps.model.pinnedEntries;
    wl_output* filterOutput = dockFilterOutput(cfg, instance.output);
    instance.lastFilterOutput = filterOutput;

    if (cfg.showRunning) {
      const auto runningIds = deps.model.platform.runningAppIds(filterOutput);
      const auto& allEntries = desktopEntries();
      const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, allEntries);

      for (const auto& run : resolvedRunning) {
        bool alreadyPresent = false;
        for (const auto& itm : itemEntries) {
          if (app_identity::desktopEntryMatchesLower(itm, run.runningLower)) {
            alreadyPresent = true;
            break;
          }
        }
        if (alreadyPresent) {
          continue;
        }

        itemEntries.push_back(run.entry);
      }
    }

    const auto activeIdLower = instance.activeAppIdLower;
    const auto runningIds = deps.model.platform.runningAppIds(filterOutput);
    const auto resolvedRunning = app_identity::resolveRunningApps(runningIds, desktopEntries());
    std::vector<std::string> runningLower;
    runningLower.reserve(resolvedRunning.size());
    for (const auto& run : resolvedRunning) {
      runningLower.push_back(StringUtils::toLower(run.entry.id));
    }

    if (cfg.launcherPosition == "start") {
      instance.row->addChild(createLauncherButton(instance, cfg, clickContext));
    }

    // Reserve up-front so emplace_back never reallocates while lambdas hold raw pointers.
    instance.items.reserve(itemEntries.size());

    for (const auto& entry : itemEntries) {
      auto& item = instance.items.emplace_back();
      item.entry = entry;
      item.idLower = StringUtils::toLower(entry.id);
      item.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
      item.active = matchesActiveApp(item, activeIdLower);
      item.running = matchesRunningApp(item, runningLower);

      const float cellMain = iSize + (2.0f * kCellPad);
      const float cellCross = iSize + (2.0f * kCellPad);
      auto areaNode = std::make_unique<InputArea>();
      if (!vert) {
        areaNode->setSize(cellMain, cellCross);
      } else {
        areaNode->setSize(cellCross, cellMain);
      }

      areaNode->addChild(
          ui::box({
              .out = &item.background,
              .fill = clearColorSpec(),
              .radius = static_cast<float>(cfg.radius),
              .width = cellMain,
              .height = cellMain,
              .configure = [](Box& box) { box.setPosition(0.0f, 0.0f); },
          })
      );

      const std::string& iconPath = [&]() -> const std::string& {
        if (!entry.icon.empty()) {
          const std::string& primary = deps.iconResolver.resolve(entry.icon, cfg.iconSize);
          if (!primary.empty()) {
            return primary;
          }
        }
        return deps.iconResolver.resolve("application-x-executable", cfg.iconSize);
      }();
      RenderContext* renderContext = &deps.renderContext;
      auto iconImg = ui::image({
          .width = iSize,
          .height = iSize,
          .configure = [renderContext, &iconPath, &cfg](Image& image) {
            if (!iconPath.empty() && renderContext != nullptr) {
              image.setSourceFile(*renderContext, iconPath, cfg.iconSize, true);
            }
            image.setPosition(kCellPad, kCellPad);
          },
      });

      if (iconImg->hasImage()) {
        item.iconImage = dynamic_cast<Image*>(areaNode->addChild(std::move(iconImg)));
      } else {
        item.iconGlyph = dynamic_cast<Glyph*>(areaNode->addChild(
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
        const bool verticalDots = shell::dock::isVerticalPosition(cfg.position);

        for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
          item.dotIndicators[dotIndex] = dynamic_cast<Box*>(areaNode->addChild(
              ui::box({
                  .fill = colorSpecFromRole(ColorRole::Secondary),
                  .radius = dot * 0.5f,
                  .width = dot,
                  .height = dot,
                  .visible = false,
                  .configure = [verticalDots, position = cfg.position, cellMain, dot](Box& box) {
                    if (verticalDots) {
                      const float x = position == "left" ? std::round(cellMain - dot - 1.0f) : 1.0f;
                      box.setPosition(x, std::round((cellMain - dot) * 0.5f));
                    } else {
                      const float y = position == "bottom" ? 1.0f : std::round(cellMain - dot - 1.0f);
                      box.setPosition(std::round((cellMain - dot) * 0.5f), y);
                    }
                  },
              })
          ));
        }
      }

      if (cfg.showInstanceCount) {
        const float bd = std::max(kBadgeMinSize, iSize * kBadgeSizeRatio);
        const float badgeX = kCellPad + iSize - (bd * 0.55f);
        const float badgeY = kCellPad - (bd * 0.45f);

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
                .maxLines = 1,
                .fontWeight = FontWeight::Bold,
                .visible = false,
            })
        );
      }

      auto* itemPtr = &item;
      auto* instPtr = &instance;

      areaNode->setOnEnter([itemPtr, instPtr](const InputArea::PointerData&) {
        if (!itemPtr->hovered) {
          itemPtr->hovered = true;
          if (itemPtr->background) {
            itemPtr->background->setFill(colorSpecFromRole(ColorRole::Hover));
          }
          if (instPtr->sceneRoot) {
            instPtr->sceneRoot->markPaintDirty();
          }
        }
      });
      areaNode->setOnLeave([itemPtr, instPtr]() {
        if (itemPtr->hovered) {
          itemPtr->hovered = false;
          if (itemPtr->background) {
            itemPtr->background->setFill(clearColorSpec());
          }
          if (instPtr->sceneRoot) {
            instPtr->sceneRoot->markPaintDirty();
          }
        }
      });
      areaNode->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
      areaNode->setOnClick([itemPtr, instPtr, clickContext](const InputArea::PointerData& d) {
        if (d.button == BTN_LEFT) {
          handleItemClick(*instPtr, *itemPtr, *clickContext);
        } else if (d.button == BTN_RIGHT && clickContext->callbacks.openItemMenu) {
          clickContext->callbacks.openItemMenu(*instPtr, *itemPtr);
        }
      });

      item.area = dynamic_cast<InputArea*>(instance.row->addChild(std::move(areaNode)));
    }

    if (cfg.launcherPosition == "end") {
      instance.row->addChild(createLauncherButton(instance, cfg, clickContext));
    }

    instance.modelSerial = deps.model.modelSerial;

    shell::dock::resizeSurface(instance, cfg, deps.model.config.config().shell.shadow);
  }

  void updateVisuals(DockInstance& instance, DockItemSceneDependencies deps) {
    const auto& cfg = deps.model.config.config().dock;

    for (auto& item : instance.items) {
      const float iconScale = item.active ? cfg.activeScale : cfg.inactiveScale;
      const float iconOpacity = item.active ? cfg.activeOpacity : cfg.inactiveOpacity;
      Node* iconNode =
          item.iconImage != nullptr ? static_cast<Node*>(item.iconImage) : static_cast<Node*>(item.iconGlyph);

      if (iconNode != nullptr) {
        if (item.visualScale < 0.0f) {
          item.visualScale = iconScale;
          iconNode->setScale(iconScale);
        } else if (std::abs(item.visualScale - iconScale) > 0.001f) {
          if (item.scaleAnimId != 0) {
            instance.animations.cancel(item.scaleAnimId);
          }
          item.scaleAnimId = instance.animations.animate(
              item.visualScale, iconScale, Style::animNormal, Easing::EaseOutCubic,
              [node = iconNode, itemPtr = &item](float value) {
                itemPtr->visualScale = value;
                node->setScale(value);
              },
              [itemPtr = &item] { itemPtr->scaleAnimId = 0; }
          );
        }

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

      const bool needsWindowCount = cfg.showDots || item.badge != nullptr;
      std::size_t count = 0;
      if (needsWindowCount) {
        const auto windows = deps.model.platform.windowsForApp(
            item.idLower, item.startupWmClassLower, dockFilterOutput(cfg, instance.output)
        );
        count = windows.size();
        item.instanceCount = count;
      }

      if (cfg.showDots) {
        const std::size_t dotCount = std::min<std::size_t>(count, 3);
        const float iSize = static_cast<float>(cfg.iconSize);
        const float cellMain = iSize + (2.0f * kCellPad);
        const float dot = std::max(kDotMinSize, std::round(iSize * kDotSizeRatio));
        const float groupLength =
            dotCount == 0 ? dot : (dot * static_cast<float>(dotCount)) + (kDotGap * static_cast<float>(dotCount - 1));
        const float groupStart = std::round((cellMain - groupLength) * 0.5f);
        const bool verticalDots = shell::dock::isVerticalPosition(cfg.position);

        for (std::size_t dotIndex = 0; dotIndex < item.dotIndicators.size(); ++dotIndex) {
          if (item.dotIndicators[dotIndex] == nullptr) {
            continue;
          }
          Box* dotNode = item.dotIndicators[dotIndex];
          const bool visible = dotIndex < dotCount;
          dotNode->setVisible(visible);
          dotNode->setFill(colorSpecFromRole(ColorRole::Secondary));
          if (visible) {
            const float main = groupStart + (static_cast<float>(dotIndex) * (dot + kDotGap));
            if (verticalDots) {
              const float x = cfg.position == "left" ? std::round(cellMain - dot - 1.0f) : 1.0f;
              dotNode->setPosition(x, main);
            } else {
              const float y = cfg.position == "bottom" ? 1.0f : std::round(cellMain - dot - 1.0f);
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
  }

  void handleItemClick(DockInstance& instance, DockItemView& item, DockItemClickContext& context) {
    if (context.callbacks.pruneCachedToplevelHandles) {
      context.callbacks.pruneCachedToplevelHandles();
    }

    auto windows = context.platform.windowsForApp(
        item.idLower, item.startupWmClassLower, dockFilterOutput(context.config.config().dock, instance.output)
    );

    if (windows.empty()) {
      wl_surface* const activationSurface = instance.surface != nullptr ? instance.surface->wlSurface() : nullptr;
      const auto options = context.callbacks.launchOptions ? context.callbacks.launchOptions(activationSurface)
                                                           : desktop_entry_launch::LaunchOptions{};
      (void)desktop_entry_launch::launchEntry(item.entry, options);
      return;
    }

    if (windows.size() == 1) {
      context.platform.activateToplevel(windows[0].handle);
      return;
    }

    zwlr_foreign_toplevel_handle_v1* activeHandle = nullptr;
    if (const auto active = context.platform.activeToplevel(); active.has_value()) {
      activeHandle = active->handle;
    }

    auto* preferredHandle = [&]() -> zwlr_foreign_toplevel_handle_v1* {
      const auto it = context.lastActiveHandleByAppIdLower.find(item.idLower);
      return it != context.lastActiveHandleByAppIdLower.end() ? it->second : nullptr;
    }();
    if (auto* nextHandle = nextActivatableWindowHandle(windows, activeHandle, preferredHandle); nextHandle != nullptr) {
      context.platform.activateToplevel(nextHandle);
    }
  }

} // namespace shell::dock
