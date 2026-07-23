#include "shell/settings/bar_widget_editor.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/scene/node.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  // Defined in settings_content_common.cpp (header not included here to avoid a makeLabel overload clash).
  [[nodiscard]] std::string formatSliderValue(double value, bool integerValue);
  [[nodiscard]] std::optional<double> parseDoubleInput(std::string_view text);

  namespace {

    struct LaneWidgetDragState {
      bool active = false;
      bool moved = false;
      float startLocalX = 0.0f;
      float startLocalY = 0.0f;
      float lastLocalX = 0.0f;
      float lastLocalY = 0.0f;
      std::optional<std::size_t> targetZoneIndex;
      std::optional<std::size_t> targetInsertionIndex;
      // Set when hovering over the middle of another loose widget: dropping forms a new group with it.
      std::optional<std::size_t> combineZoneIndex;
      std::optional<std::size_t> combineItemIndex;
      // The card currently highlighted as a combine target (so it can be reset).
      std::optional<std::size_t> highlightZoneIndex;
      std::optional<std::size_t> highlightItemIndex;
    };

    // A drop zone is either a bar lane (entries = widget refs / group tokens) or a group's member list.
    // Both lanes and group containers register as zones so widgets can be dragged into and out of groups.
    struct DropZone {
      bool isGroup = false;
      std::vector<std::string> lanePath; // when !isGroup
      std::string groupId;               // when isGroup
      std::vector<std::string> items;    // lane entries or group members (snapshot)
      Flex* container = nullptr;
      Box* indicator = nullptr;
      std::shared_ptr<std::vector<Flex*>> itemNodes;
    };

    std::unique_ptr<Label> makeLabel(
        std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal
    ) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .fontWeight = fontWeight,
          .color = color,
      });
    }

    std::unique_ptr<Glyph> makeGlyph(std::string_view name, float glyphSize, const ColorSpec& color) {
      return ui::glyph({
          .glyph = std::string(name),
          .glyphSize = glyphSize,
          .color = color,
      });
    }

    std::unique_ptr<Node> makeMiniSectionHeader(std::string_view title, float scale, bool withSeparator = true) {
      auto header = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .configure = [scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0f, 0.0f, 0.0f); },
      });
      if (withSeparator) {
        header->addChild(ui::separator());
      }
      header->addChild(
          makeLabel(title, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
      );
      return header;
    }

    std::string widgetSettingGroupTitle(std::string_view groupKey) {
      return i18n::tr("settings.entities.widget.settings.groups." + std::string(groupKey));
    }

    enum class PathBrowseKind : std::uint8_t {
      File,
      Folder,
    };

    void applyPathDialogStartValue(FileDialogOptions& options, const std::string& currentValue, PathBrowseKind kind) {
      if (currentValue.empty()) {
        return;
      }

      const std::filesystem::path current(currentValue);
      std::error_code ec;
      if (kind == PathBrowseKind::Folder
          && std::filesystem::exists(current, ec)
          && std::filesystem::is_directory(current, ec)) {
        options.startDirectory = current;
        return;
      }
      if (kind == PathBrowseKind::File
          && std::filesystem::exists(current, ec)
          && std::filesystem::is_regular_file(current, ec)) {
        options.startDirectory = current.parent_path();
        options.defaultFilename = current.filename().string();
        return;
      }
      if (current.has_parent_path()
          && std::filesystem::exists(current.parent_path(), ec)
          && std::filesystem::is_directory(current.parent_path(), ec)) {
        options.startDirectory = current.parent_path();
      }
    }

    std::unique_ptr<Node> makePathBrowseControl(
        const BarWidgetEditorContext& ctx, std::vector<std::string> path, std::string currentValue, std::string glyph,
        FileDialogOptions options, PathBrowseKind kind, std::string dialogStartValue = {}
    ) {
      if (dialogStartValue.empty()) {
        dialogStartValue = currentValue;
      }

      auto textNode = ctx.makeText(currentValue, {}, path);
      return ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * ctx.scale,
          },
          std::move(textNode),
          ui::button({
              .glyph = std::move(glyph),
              .glyphSize = Style::fontSizeBody * ctx.scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeight * ctx.scale,
              .minHeight = Style::controlHeight * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusMd(ctx.scale),
              .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path = std::move(path),
                          options = std::move(options), kind, dialogStartValue = std::move(dialogStartValue)]() {
                FileDialogOptions dialogOptions = options;
                applyPathDialogStartValue(dialogOptions, dialogStartValue, kind);
                (void)FileDialog::open(
                    std::move(dialogOptions),
                    [setOverride, requestRebuild, path](std::optional<std::filesystem::path> picked) {
                      if (!picked.has_value()) {
                        return;
                      }
                      setOverride(path, picked->string());
                      if (requestRebuild) {
                        requestRebuild();
                      }
                    }
                );
              },
          })
      );
    }

    std::string pathKey(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

    std::vector<std::string> pathWithLastSegment(std::vector<std::string> path, std::string segment) {
      if (!path.empty()) {
        path.back() = std::move(segment);
      }
      return path;
    }

    std::string laneLabel(std::string_view lane) {
      if (lane == "start") {
        return i18n::tr("settings.entities.widget.lanes.start");
      }
      if (lane == "center") {
        return i18n::tr("settings.entities.widget.lanes.center");
      }
      if (lane == "end") {
        return i18n::tr("settings.entities.widget.lanes.end");
      }
      return std::string(lane);
    }

    std::vector<std::string> barWidgetItemsForPath(const Config& cfg, const std::vector<std::string>& path) {
      if (!isBarWidgetListPath(path) || path.size() < 3) {
        return {};
      }

      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return {};
      }

      const auto& lane = path.back();
      if (path.size() >= 5 && path[2] == "monitor") {
        const auto* ovr = findMonitorOverride(*bar, path[3]);
        if (ovr != nullptr) {
          if (lane == "start") {
            return ovr->startWidgets.value_or(bar->startWidgets);
          }
          if (lane == "center") {
            return ovr->centerWidgets.value_or(bar->centerWidgets);
          }
          if (lane == "end") {
            return ovr->endWidgets.value_or(bar->endWidgets);
          }
        }
      }

      if (lane == "start") {
        return bar->startWidgets;
      }
      if (lane == "center") {
        return bar->centerWidgets;
      }
      if (lane == "end") {
        return bar->endWidgets;
      }
      return {};
    }

    bool isMonitorWidgetListPath(const std::vector<std::string>& path) {
      return isBarWidgetListPath(path) && path.size() >= 5 && path[2] == "monitor";
    }

    bool monitorWidgetListHasExplicitValue(const Config& cfg, const std::vector<std::string>& path) {
      if (!isMonitorWidgetListPath(path)) {
        return true;
      }

      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return true;
      }
      const auto* ovr = findMonitorOverride(*bar, path[3]);
      if (ovr == nullptr) {
        return false;
      }

      const auto& lane = path.back();
      if (lane == "start") {
        return ovr->startWidgets.has_value();
      }
      if (lane == "center") {
        return ovr->centerWidgets.has_value();
      }
      if (lane == "end") {
        return ovr->endWidgets.has_value();
      }
      return true;
    }

    // Compact kind indicator used on lane cards in place of the text badge.
    std::string_view widgetBadgeGlyph(WidgetReferenceKind kind) {
      switch (kind) {
      case WidgetReferenceKind::BuiltIn:
        return "box";
      case WidgetReferenceKind::Named:
        return "tag";
      case WidgetReferenceKind::Plugin:
        return "puzzle";
      case WidgetReferenceKind::Unknown:
        return "help-circle";
      }
      return "help-circle";
    }

    ColorSpec widgetBadgeGlyphColor(WidgetReferenceKind kind) {
      switch (kind) {
      case WidgetReferenceKind::BuiltIn:
        return colorSpecFromRole(ColorRole::Primary);
      case WidgetReferenceKind::Named:
      case WidgetReferenceKind::Plugin:
        return colorSpecFromRole(ColorRole::Secondary);
      case WidgetReferenceKind::Unknown:
        return colorSpecFromRole(ColorRole::Error);
      }
      return colorSpecFromRole(ColorRole::OnSurfaceVariant);
    }

    void collectWidgetReferenceNames(const std::vector<std::string>& widgets, std::unordered_set<std::string>& seen) {
      for (const auto& widget : widgets) {
        seen.insert(widget);
      }
    }

    bool widgetReferenceNameExists(const Config& cfg, std::string_view name) {
      const std::string key(name);
      if (isBuiltInWidgetType(name) || cfg.widgets.contains(key)) {
        return true;
      }

      std::unordered_set<std::string> seen;
      for (const auto& bar : cfg.bars) {
        collectWidgetReferenceNames(bar.startWidgets, seen);
        collectWidgetReferenceNames(bar.centerWidgets, seen);
        collectWidgetReferenceNames(bar.endWidgets, seen);
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.startWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.startWidgets, seen);
          }
          if (ovr.centerWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.centerWidgets, seen);
          }
          if (ovr.endWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.endWidgets, seen);
          }
        }
      }
      return seen.contains(key);
    }

    bool removeWidgetReference(std::vector<std::string>& items, std::string_view widgetName) {
      const auto oldSize = items.size();
      const std::string key(widgetName);
      std::erase(items, key);
      return items.size() != oldSize;
    }

    void appendReferenceRemoval(
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, std::vector<std::string> path,
        std::vector<std::string> items, std::string_view widgetName
    ) {
      if (removeWidgetReference(items, widgetName)) {
        overrides.emplace_back(std::move(path), std::move(items));
      }
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    widgetReferenceRemovalOverrides(const Config& cfg, std::string_view widgetName) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      for (const auto& bar : cfg.bars) {
        appendReferenceRemoval(overrides, {"bar", bar.name, "start"}, bar.startWidgets, widgetName);
        appendReferenceRemoval(overrides, {"bar", bar.name, "center"}, bar.centerWidgets, widgetName);
        appendReferenceRemoval(overrides, {"bar", bar.name, "end"}, bar.endWidgets, widgetName);

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string> prefix = {"bar", bar.name, "monitor", ovr.match};
          if (ovr.startWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "start"}, *ovr.startWidgets, widgetName
            );
          }
          if (ovr.centerWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "center"}, *ovr.centerWidgets, widgetName
            );
          }
          if (ovr.endWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "end"}, *ovr.endWidgets, widgetName
            );
          }
        }
      }
      return overrides;
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    widgetReferenceRenameOverrides(const Config& cfg, std::string_view oldName, std::string_view newName) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      for (const auto& bar : cfg.bars) {
        auto appendRename = [&](std::vector<std::string> path, std::vector<std::string> items) {
          bool changed = false;
          for (auto& item : items) {
            if (item == oldName) {
              item = std::string(newName);
              changed = true;
            }
          }
          if (changed) {
            overrides.emplace_back(std::move(path), std::move(items));
          }
        };

        appendRename({"bar", bar.name, "start"}, bar.startWidgets);
        appendRename({"bar", bar.name, "center"}, bar.centerWidgets);
        appendRename({"bar", bar.name, "end"}, bar.endWidgets);

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string> prefix = {"bar", bar.name, "monitor", ovr.match};
          if (ovr.startWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "start"}, *ovr.startWidgets);
          }
          if (ovr.centerWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "center"}, *ovr.centerWidgets);
          }
          if (ovr.endWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "end"}, *ovr.endWidgets);
          }
        }
      }
      return overrides;
    }

    bool isNamedWidgetInstance(const Config& cfg, std::string_view widgetName) {
      return cfg.widgets.contains(std::string(widgetName)) && !isBuiltInWidgetType(widgetName);
    }

    bool isGuiManagedNamedWidgetInstance(const BarWidgetEditorContext& ctx, std::string_view widgetName) {
      return isNamedWidgetInstance(ctx.config, widgetName)
          && ctx.configService != nullptr
          && ctx.configService->hasOverride({"widget", std::string(widgetName)});
    }

    bool widgetHasPlacementAfterLaneEdit(
        const Config& cfg, const std::vector<std::string>& editedLanePath,
        const std::vector<std::string>& editedLaneItems, std::string_view widgetName
    ) {
      const auto editedItems = [&](const std::vector<std::string>& path,
                                   const std::vector<std::string>& items) -> const std::vector<std::string>& {
        return path == editedLanePath ? editedLaneItems : items;
      };
      const auto scopeContains = [&](const std::vector<std::string>& start, const std::vector<std::string>& center,
                                     const std::vector<std::string>& end,
                                     const std::vector<BarCapsuleGroupStyle>& groups) {
        if (std::ranges::contains(start, widgetName)
            || std::ranges::contains(center, widgetName)
            || std::ranges::contains(end, widgetName)) {
          return true;
        }
        for (const auto& group : groups) {
          if (!std::ranges::contains(group.members, widgetName)) {
            continue;
          }
          const std::string token = makeCapsuleGroupToken(group.id);
          if (std::ranges::contains(start, token)
              || std::ranges::contains(center, token)
              || std::ranges::contains(end, token)) {
            return true;
          }
        }
        return false;
      };

      for (const auto& bar : cfg.bars) {
        const std::vector<std::string>& baseStart = editedItems({"bar", bar.name, "start"}, bar.startWidgets);
        const std::vector<std::string>& baseCenter = editedItems({"bar", bar.name, "center"}, bar.centerWidgets);
        const std::vector<std::string>& baseEnd = editedItems({"bar", bar.name, "end"}, bar.endWidgets);
        if (scopeContains(baseStart, baseCenter, baseEnd, bar.widgetCapsuleGroups)) {
          return true;
        }

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string>& monitorStart = ovr.startWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "start"}, *ovr.startWidgets)
              : baseStart;
          const std::vector<std::string>& monitorCenter = ovr.centerWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "center"}, *ovr.centerWidgets)
              : baseCenter;
          const std::vector<std::string>& monitorEnd = ovr.endWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "end"}, *ovr.endWidgets)
              : baseEnd;
          const auto& groups = ovr.widgetCapsuleGroups.has_value() ? *ovr.widgetCapsuleGroups : bar.widgetCapsuleGroups;
          if (scopeContains(monitorStart, monitorCenter, monitorEnd, groups)) {
            return true;
          }
        }
      }
      return false;
    }

    bool isValidWidgetInstanceId(std::string_view id) {
      if (id.empty()) {
        return false;
      }
      for (char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
          return false;
        }
      }
      return true;
    }

    bool canRenameWidgetInstance(const Config& cfg, std::string_view oldName, std::string_view newName) {
      return isValidWidgetInstanceId(newName) && oldName != newName && !widgetReferenceNameExists(cfg, newName);
    }

    const BarConfig* barForLanePath(const Config& cfg, const std::vector<std::string>& path) {
      if (path.size() < 2 || path[0] != "bar") {
        return nullptr;
      }
      return findBar(cfg, path[1]);
    }

    const BarMonitorOverride* monitorOverrideForLanePath(const Config& cfg, const std::vector<std::string>& path) {
      if (!isMonitorWidgetListPath(path)) {
        return nullptr;
      }
      const BarConfig* bar = barForLanePath(cfg, path);
      return bar != nullptr ? findMonitorOverride(*bar, path[3]) : nullptr;
    }

    std::vector<std::string> capsuleGroupPathForLanePath(const std::vector<std::string>& lanePath) {
      if (isMonitorWidgetListPath(lanePath)) {
        return {"bar", lanePath[1], "monitor", lanePath[3], "capsule_group"};
      }
      if (lanePath.size() >= 2 && lanePath[0] == "bar") {
        return {"bar", lanePath[1], "capsule_group"};
      }
      return {};
    }

    std::vector<BarCapsuleGroupStyle>
    capsuleGroupsForLanePath(const Config& cfg, const std::vector<std::string>& lanePath) {
      const BarConfig* bar = barForLanePath(cfg, lanePath);
      if (bar == nullptr) {
        return {};
      }
      const BarMonitorOverride* ovr = monitorOverrideForLanePath(cfg, lanePath);
      if (ovr != nullptr && ovr->widgetCapsuleGroups.has_value()) {
        return *ovr->widgetCapsuleGroups;
      }
      return bar->widgetCapsuleGroups;
    }

    const BarCapsuleGroupStyle*
    findCapsuleGroupStyle(const std::vector<BarCapsuleGroupStyle>& groups, std::string_view id) {
      const auto it = std::ranges::find(groups, id, &BarCapsuleGroupStyle::id);
      return it != groups.end() ? &*it : nullptr;
    }

    // Smallest unused `g<N>` id within an owner scope's existing groups.
    std::string nextCapsuleGroupId(const std::vector<BarCapsuleGroupStyle>& groups) {
      int n = 1;
      std::string candidate = "g" + std::to_string(n);
      while (std::ranges::contains(groups, candidate, &BarCapsuleGroupStyle::id)) {
        candidate = "g" + std::to_string(++n);
      }
      return candidate;
    }

    // New group style seeded from the effective bar/monitor capsule defaults.
    BarCapsuleGroupStyle
    seedCapsuleGroupStyle(const Config& cfg, const std::vector<std::string>& lanePath, std::string id) {
      BarCapsuleGroupStyle group;
      group.id = std::move(id);
      const BarConfig* bar = barForLanePath(cfg, lanePath);
      if (bar == nullptr) {
        return group;
      }
      group.fill = bar->widgetCapsuleFill;
      group.borderSpecified = bar->widgetCapsuleBorderSpecified;
      group.border = bar->widgetCapsuleBorder;
      group.foreground = bar->widgetCapsuleForeground;
      group.padding = bar->widgetCapsulePadding;
      if (bar->widgetCapsuleRadius.has_value()) {
        group.radius = static_cast<float>(*bar->widgetCapsuleRadius);
      }
      group.opacity = bar->widgetCapsuleOpacity;

      const BarMonitorOverride* ovr = monitorOverrideForLanePath(cfg, lanePath);
      if (ovr == nullptr) {
        return group;
      }
      if (ovr->widgetCapsuleFill.has_value()) {
        group.fill = *ovr->widgetCapsuleFill;
      }
      if (ovr->widgetCapsuleBorderSpecified) {
        group.borderSpecified = true;
        group.border = ovr->widgetCapsuleBorder;
      }
      if (ovr->widgetCapsuleForeground.has_value()) {
        group.foreground = *ovr->widgetCapsuleForeground;
      }
      if (ovr->widgetCapsulePadding.has_value()) {
        group.padding = std::clamp(static_cast<float>(*ovr->widgetCapsulePadding), 0.0f, 48.0f);
      }
      if (ovr->widgetCapsuleRadius.has_value()) {
        group.radius = static_cast<float>(std::clamp(*ovr->widgetCapsuleRadius, 0.0, 80.0));
      }
      if (ovr->widgetCapsuleOpacity.has_value()) {
        group.opacity = std::clamp(static_cast<float>(*ovr->widgetCapsuleOpacity), 0.0f, 1.0f);
      }
      return group;
    }

    std::size_t insertionIndexForSceneY(float sceneY, const std::vector<Flex*>& itemNodes) {
      for (std::size_t i = 0; i < itemNodes.size(); ++i) {
        const auto* item = itemNodes[i];
        if (item == nullptr) {
          continue;
        }
        float ignoredX = 0.0f;
        float itemY = 0.0f;
        Node::absolutePosition(item, ignoredX, itemY);
        if (sceneY < itemY + item->height() * 0.5f) {
          return i;
        }
      }
      return itemNodes.size();
    }

    bool insertionWouldNotMove(
        std::size_t sourceZoneIndex, std::size_t targetZoneIndex, std::size_t fromIndex, std::size_t insertionIndex
    ) {
      return sourceZoneIndex == targetZoneIndex && (insertionIndex == fromIndex || insertionIndex == fromIndex + 1);
    }

    // Innermost zone containing the point. Group zones win over the lane that encloses them.
    std::optional<std::size_t> zoneAtScenePoint(const std::vector<DropZone>& zones, float sceneX, float sceneY) {
      std::optional<std::size_t> laneHit;
      for (std::size_t i = 0; i < zones.size(); ++i) {
        const auto* container = zones[i].container;
        if (container == nullptr) {
          continue;
        }
        float zoneX = 0.0f;
        float zoneY = 0.0f;
        Node::absolutePosition(container, zoneX, zoneY);
        const bool inside = sceneX >= zoneX
            && sceneX < zoneX + container->width()
            && sceneY >= zoneY
            && sceneY < zoneY + container->height();
        if (!inside) {
          continue;
        }
        if (zones[i].isGroup) {
          return i;
        }
        laneHit = i;
      }
      return laneHit;
    }

    void hideDropIndicators(const std::vector<DropZone>& zones) {
      for (const auto& zone : zones) {
        if (zone.indicator != nullptr) {
          zone.indicator->setVisible(false);
        }
      }
    }

    // Applies a drag move from (srcZone, srcIdx) to (dstZone, insertionIndex) by writing the affected lane
    // vectors and/or the bar's capsule-group vector. Group member edits funnel through one group-vector write.
    void performZoneMove(
        const Config& cfg, const std::vector<std::string>& laneListPath, const std::vector<DropZone>& zones,
        std::size_t srcZone, std::size_t srcIdx, std::size_t dstZone, std::size_t insertionIndex,
        const std::function<void(std::vector<std::string>, ConfigOverrideValue)>& setOverride,
        const std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>& setOverrides
    ) {
      if (srcZone >= zones.size() || dstZone >= zones.size()) {
        return;
      }
      std::vector<std::string> srcItems = zones[srcZone].items;
      if (srcIdx >= srcItems.size()) {
        return;
      }
      const std::string moving = srcItems[srcIdx];
      const bool sameZone = srcZone == dstZone;
      // No nesting: a group token cannot be dropped inside a group.
      if (zones[dstZone].isGroup && isCapsuleGroupToken(moving)) {
        return;
      }
      if (sameZone && insertionWouldNotMove(srcZone, dstZone, srcIdx, insertionIndex)) {
        return;
      }

      srcItems.erase(srcItems.begin() + static_cast<std::ptrdiff_t>(srcIdx));
      std::vector<std::string> dstItems = sameZone ? srcItems : zones[dstZone].items;
      std::size_t insert = insertionIndex;
      if (sameZone && insertionIndex > srcIdx) {
        --insert;
      }
      insert = std::min(insert, dstItems.size());
      dstItems.insert(dstItems.begin() + static_cast<std::ptrdiff_t>(insert), moving);

      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(cfg, laneListPath);
      bool groupsTouched = false;
      // Lane edits keyed by zone index, so a later empty-group cleanup can also drop a token from a lane.
      std::vector<std::pair<std::size_t, std::vector<std::string>>> laneEdits;
      const auto setLane = [&](std::size_t zoneIndex, std::vector<std::string> items) {
        for (auto& edit : laneEdits) {
          if (edit.first == zoneIndex) {
            edit.second = std::move(items);
            return;
          }
        }
        laneEdits.emplace_back(zoneIndex, std::move(items));
      };
      const auto laneItemsFor = [&](std::size_t zoneIndex) {
        for (const auto& edit : laneEdits) {
          if (edit.first == zoneIndex) {
            return edit.second;
          }
        }
        return zones[zoneIndex].items;
      };
      const auto applyZone = [&](std::size_t zoneIndex, const std::vector<std::string>& items) {
        const DropZone& zone = zones[zoneIndex];
        if (zone.isGroup) {
          for (auto& g : groups) {
            if (g.id == zone.groupId) {
              g.members = items;
              break;
            }
          }
          groupsTouched = true;
        } else {
          setLane(zoneIndex, items);
        }
      };
      if (sameZone) {
        applyZone(srcZone, dstItems);
      } else {
        applyZone(srcZone, srcItems);
        applyZone(dstZone, dstItems);
      }

      // Dragging the last member out empties a group: drop it and its lane token.
      if (groupsTouched) {
        for (const auto& g : groups) {
          if (!g.members.empty()) {
            continue;
          }
          const std::string token = makeCapsuleGroupToken(g.id);
          for (std::size_t zi = 0; zi < zones.size(); ++zi) {
            if (zones[zi].isGroup) {
              continue;
            }
            std::vector<std::string> items = laneItemsFor(zi);
            const auto it = std::ranges::find(items, token);
            if (it != items.end()) {
              items.erase(it);
              setLane(zi, std::move(items));
            }
          }
        }
        std::vector<BarCapsuleGroupStyle> kept;
        for (auto& g : groups) {
          if (!g.members.empty()) {
            kept.push_back(std::move(g));
          }
        }
        groups.swap(kept);
      }

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
      batch.reserve(laneEdits.size());
      for (const auto& edit : laneEdits) {
        batch.emplace_back(zones[edit.first].lanePath, edit.second);
      }
      if (groupsTouched) {
        const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
        if (!groupPath.empty()) {
          batch.emplace_back(groupPath, groups);
        }
      }
      if (batch.size() == 1) {
        setOverride(batch[0].first, batch[0].second);
      } else if (!batch.empty()) {
        setOverrides(batch);
      }
    }

    // Index of the item under sceneY, plus whether the pointer is in its middle band (a "combine" gesture
    // rather than an insertion between items).
    std::optional<std::pair<std::size_t, bool>> hoveredItemBand(float sceneY, const std::vector<Flex*>& itemNodes) {
      for (std::size_t i = 0; i < itemNodes.size(); ++i) {
        const auto* node = itemNodes[i];
        if (node == nullptr) {
          continue;
        }
        float nodeX = 0.0f;
        float nodeY = 0.0f;
        Node::absolutePosition(node, nodeX, nodeY);
        const float h = node->height();
        if (h > 0.0f && sceneY >= nodeY && sceneY < nodeY + h) {
          const float rel = (sceneY - nodeY) / h;
          return std::make_pair(i, rel > 0.3f && rel < 0.7f);
        }
      }
      return std::nullopt;
    }

    // Toggles the combine-target outline on a loose widget card (reset matches makeWidgetCard's default border).
    void
    setCardCombineHighlight(const std::vector<DropZone>& zones, std::size_t zoneIndex, std::size_t itemIndex, bool on) {
      if (zoneIndex >= zones.size()
          || zones[zoneIndex].itemNodes == nullptr
          || itemIndex >= zones[zoneIndex].itemNodes->size()) {
        return;
      }
      Flex* card = (*zones[zoneIndex].itemNodes)[itemIndex];
      if (card == nullptr) {
        return;
      }
      if (on) {
        card->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 2.0f);
      } else {
        card->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      }
    }

    // Creates a new group from two loose widgets (the dragged one dropped onto the target). The target keeps
    // its lane position (now a group token); the dragged widget is pulled from its source lane.
    void createGroupByCombine(
        const Config& cfg, const std::vector<DropZone>& zones, std::size_t draggedZone, std::size_t draggedIdx,
        std::size_t targetZone, std::size_t targetIdx,
        const std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>& setOverrides
    ) {
      if (draggedZone >= zones.size()
          || targetZone >= zones.size()
          || zones[draggedZone].isGroup
          || zones[targetZone].isGroup) {
        return;
      }
      const DropZone& dz = zones[draggedZone];
      const DropZone& tz = zones[targetZone];
      if (draggedIdx >= dz.items.size() || targetIdx >= tz.items.size()) {
        return;
      }
      const std::string draggedName = dz.items[draggedIdx];
      const std::string targetName = tz.items[targetIdx];
      if (isCapsuleGroupToken(draggedName) || isCapsuleGroupToken(targetName)) {
        return;
      }
      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(dz.lanePath);
      if (groupPath.empty()) {
        return;
      }
      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(cfg, dz.lanePath);
      const std::string newId = nextCapsuleGroupId(groups);
      BarCapsuleGroupStyle newGroup = seedCapsuleGroupStyle(cfg, dz.lanePath, newId);
      newGroup.members = {targetName, draggedName};
      groups.push_back(std::move(newGroup));
      const std::string token = makeCapsuleGroupToken(newId);

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
      if (draggedZone == targetZone) {
        std::vector<std::string> lane;
        lane.reserve(dz.items.size());
        for (std::size_t k = 0; k < dz.items.size(); ++k) {
          if (k == draggedIdx) {
            continue;
          }
          if (k == targetIdx) {
            lane.push_back(token);
            continue;
          }
          lane.push_back(dz.items[k]);
        }
        batch.emplace_back(dz.lanePath, lane);
      } else {
        std::vector<std::string> draggedLane = dz.items;
        draggedLane.erase(draggedLane.begin() + static_cast<std::ptrdiff_t>(draggedIdx));
        std::vector<std::string> targetLane = tz.items;
        targetLane[targetIdx] = token;
        batch.emplace_back(dz.lanePath, draggedLane);
        batch.emplace_back(tz.lanePath, targetLane);
      }
      batch.emplace_back(groupPath, groups);
      setOverrides(batch);
    }

    void updateDropIndicator(
        Box& indicator, const Flex& lane, const std::vector<Flex*>& itemNodes, std::size_t insertionIndex, float scale
    ) {
      if (insertionIndex > itemNodes.size()) {
        indicator.setVisible(false);
        return;
      }

      const float x = Style::spaceSm * scale;
      const float width = std::max(1.0f, lane.width() - Style::spaceSm * scale * 2.0f);
      const float gapHalf = Style::spaceXs * scale * 0.5f;
      float y = Style::controlHeightSm * scale + Style::spaceSm * scale;
      if (!itemNodes.empty()) {
        if (insertionIndex == itemNodes.size()) {
          const auto* target = itemNodes.back();
          y = target != nullptr ? target->y() + target->height() + gapHalf : y;
        } else {
          const auto* target = itemNodes[insertionIndex];
          y = target != nullptr ? target->y() - gapHalf : y;
        }
      }

      indicator.setPosition(x, y);
      indicator.setFrameSize(width, std::max(2.0f, 3.0f * scale));
      indicator.setVisible(true);
    }

    std::vector<std::string> widgetSettingPath(std::string widgetName, std::string settingKey) {
      return {"widget", std::move(widgetName), std::move(settingKey)};
    }

    WidgetSettingValue
    widgetSettingValue(const Config& cfg, std::string_view widgetName, const WidgetSettingSpec& spec) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(spec.schema.key); settingIt != it->second.settings.end()) {
          return settingIt->second;
        }
      }
      return spec.schema.defaultValue;
    }

    std::string settingCurrentString(
        const Config& cfg, std::string_view widgetName, const std::string& key,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(key); settingIt != it->second.settings.end()) {
          if (const auto* s = std::get_if<std::string>(&settingIt->second)) {
            return *s;
          }
          if (const auto* i = std::get_if<std::int64_t>(&settingIt->second)) {
            return std::to_string(*i);
          }
          if (const auto* b = std::get_if<bool>(&settingIt->second)) {
            return *b ? "true" : "false";
          }
        }
      }
      for (const auto& s : allSpecs) {
        if (s.schema.key == key) {
          if (const auto* str = std::get_if<std::string>(&s.schema.defaultValue)) {
            return *str;
          }
          if (const auto* i = std::get_if<std::int64_t>(&s.schema.defaultValue)) {
            return std::to_string(*i);
          }
          if (const auto* b = std::get_if<bool>(&s.schema.defaultValue)) {
            return *b ? "true" : "false";
          }
          break;
        }
      }
      return {};
    }

    [[nodiscard]] bool isBarHorizontal(const Config& cfg, std::string_view barName) {
      const BarConfig* bar = findBar(cfg, barName);
      if (bar == nullptr) {
        return true;
      }
      return bar->position != "left" && bar->position != "right";
    }

    bool isSettingVisible(
        const Config& cfg, std::string_view widgetName, const WidgetSettingSpec& spec,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (!spec.visibleWhen.has_value()) {
        return true;
      }
      auto settingValueForKey = [&](const std::string& key) -> WidgetSettingValue {
        if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
          if (const auto settingIt = it->second.settings.find(key); settingIt != it->second.settings.end()) {
            return settingIt->second;
          }
        }
        for (const auto& s : allSpecs) {
          if (s.schema.key == key) {
            return s.schema.defaultValue;
          }
        }
        return {};
      };
      auto matches = [&](const WidgetSettingVisibilityCondition& condition) {
        const WidgetSettingValue value = settingValueForKey(condition.key);
        if (condition.nonEmpty) {
          if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
            return !list->empty();
          }
          if (const auto* str = std::get_if<std::string>(&value)) {
            return !str->empty();
          }
          return false;
        }
        const auto currentValue = settingCurrentString(cfg, widgetName, condition.key, allSpecs);
        for (const auto& v : condition.values) {
          if (v == currentValue) {
            return true;
          }
        }
        return false;
      };
      for (const auto& condition : spec.visibleWhen->all) {
        if (!matches(condition)) {
          return false;
        }
      }
      if (spec.visibleWhen->any.empty()) {
        return true;
      }
      for (const auto& condition : spec.visibleWhen->any) {
        if (matches(condition)) {
          return true;
        }
      }
      return false;
    }

    bool settingValueAsBool(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<bool>(&value)) {
        return *v;
      }
      return false;
    }

    std::int64_t settingValueAsInt(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*v));
      }
      return std::int64_t{0};
    }

    double settingValueAsDouble(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<double>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*v);
      }
      return 0.0;
    }

    std::optional<double>
    widgetSettingOptionalDouble(const Config& cfg, std::string_view widgetName, const std::string& key) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(key); settingIt != it->second.settings.end()) {
          if (const auto* v = std::get_if<double>(&settingIt->second)) {
            return *v;
          }
          if (const auto* v = std::get_if<std::int64_t>(&settingIt->second)) {
            return static_cast<double>(*v);
          }
        }
      }
      return std::nullopt;
    }

    std::optional<int>
    widgetSettingOptionalStepperValue(const Config& cfg, std::string_view widgetName, const std::string& key) {
      const auto value = widgetSettingOptionalDouble(cfg, widgetName, key);
      if (!value.has_value()) {
        return std::nullopt;
      }
      return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
    }

    int inheritedCapsuleRadiusForLane(const Config& cfg, const std::vector<std::string>& lanePath) {
      if (lanePath.size() < 2 || lanePath[0] != "bar") {
        return 8;
      }
      const BarConfig* bar = findBar(cfg, lanePath[1]);
      if (bar == nullptr) {
        return 8;
      }
      if (isMonitorWidgetListPath(lanePath) && lanePath.size() >= 4) {
        if (const auto* ovr = findMonitorOverride(*bar, lanePath[3]);
            ovr != nullptr && ovr->widgetCapsuleRadius.has_value()) {
          return std::clamp(static_cast<int>(std::lround(*ovr->widgetCapsuleRadius)), 0, 80);
        }
      }
      if (bar->widgetCapsuleRadius.has_value()) {
        return std::clamp(static_cast<int>(std::lround(*bar->widgetCapsuleRadius)), 0, 80);
      }
      return 8;
    }

    std::string settingValueAsString(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::string>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*v);
      }
      return {};
    }

    std::string widgetLabelFontWeightSelectedValue(const Config& cfg, std::string_view widgetName) {
      const auto widgetIt = cfg.widgets.find(std::string(widgetName));
      if (widgetIt == cfg.widgets.end()) {
        return {};
      }
      const auto settingIt = widgetIt->second.settings.find("font_weight");
      if (settingIt == widgetIt->second.settings.end()) {
        return {};
      }
      return settingValueAsString(settingIt->second);
    }

    // Effective typeface for the widget's labels: its own font_family override, else the hosting bar's
    // font_family, else the global shell font. Used to list only the weights the real font provides.
    std::string widgetResolvedFontFamily(const Config& cfg, std::string_view widgetName) {
      if (const auto widgetIt = cfg.widgets.find(std::string(widgetName)); widgetIt != cfg.widgets.end()) {
        const auto it = widgetIt->second.settings.find("font_family");
        if (it != widgetIt->second.settings.end()) {
          const std::string family = settingValueAsString(it->second);
          if (!family.empty()) {
            return family;
          }
        }
      }
      const auto inLane = [&](const std::vector<std::string>& lane) { return std::ranges::contains(lane, widgetName); };
      for (const BarConfig& bar : cfg.bars) {
        if (inLane(bar.startWidgets) || inLane(bar.centerWidgets) || inLane(bar.endWidgets)) {
          if (bar.fontFamily && !bar.fontFamily->empty()) {
            return *bar.fontFamily;
          }
          break;
        }
      }
      return cfg.shell.fontFamily;
    }

    std::vector<std::string> settingValueAsStringList(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::vector<std::string>>(&value)) {
        return *v;
      }
      return {};
    }

    std::string settingValueAsDisplayString(const WidgetSettingValue& value) {
      return std::visit(
          [](const auto& concrete) -> std::string {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, bool>) {
              return concrete ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
              return std::to_string(concrete);
            } else if constexpr (std::is_same_v<T, double>) {
              return std::format("{}", concrete);
            } else if constexpr (std::is_same_v<T, std::string>) {
              return "\"" + concrete + "\"";
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
              std::string out = "[";
              for (std::size_t i = 0; i < concrete.size(); ++i) {
                if (i > 0) {
                  out += ", ";
                }
                out += "\"" + concrete[i] + "\"";
              }
              out += "]";
              return out;
            } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
              std::vector<std::string> keys;
              keys.reserve(concrete.size());
              for (const auto& [key, mapValue] : concrete) {
                (void)mapValue;
                keys.push_back(key);
              }
              std::ranges::sort(keys);
              std::string out = "{";
              for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i > 0) {
                  out += ", ";
                }
                out += "\"" + keys[i] + "\" = \"" + concrete.at(keys[i]) + "\"";
              }
              out += "}";
              return out;
            }
          },
          value
      );
    }

    [[nodiscard]] bool workspacesCompactStyleEnabled(
        const Config& cfg, std::string_view widgetName, const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      const std::string style = settingCurrentString(cfg, widgetName, "style", allSpecs);
      return style == "minimal" || style == "focus_hint";
    }

    SelectSetting workspacesStyleSelectSetting(
        const BarWidgetEditorContext& ctx, std::string_view widgetName, const WidgetSettingSpec& styleSpec,
        const std::vector<WidgetSettingSpec>& allSpecs, std::string selectedValue
    ) {
      std::vector<SelectOption> options;
      options.reserve(styleSpec.options.size());
      for (const auto& option : styleSpec.options) {
        options.push_back(
            SelectOption{option.value, styleSpec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
        );
      }
      SelectSetting selectSetting{std::move(options), std::move(selectedValue)};
      selectSetting.segmented = styleSpec.segmented;
      const ConfigService* configService = ctx.configService;
      selectSetting.groupedCommit = [configService, widgetName = std::string(widgetName),
                                     allSpecs](std::string_view value, const std::vector<std::string>& path) {
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
        overrides.emplace_back(path, ConfigOverrideValue{std::string(value)});
        if ((value == "minimal" || value == "focus_hint")
            && configService != nullptr
            && settingCurrentString(configService->config(), widgetName, "display", allSpecs) == "none") {
          overrides.emplace_back(widgetSettingPath(widgetName, "display"), ConfigOverrideValue{std::string("id")});
        }
        return overrides;
      };
      return selectSetting;
    }

    SelectSetting workspacesDisplaySelectSetting(
        const BarWidgetEditorContext& ctx, std::string_view widgetName, const WidgetSettingSpec& displaySpec,
        const std::vector<WidgetSettingSpec>& allSpecs, std::string selectedValue
    ) {
      const bool compactStyle = workspacesCompactStyleEnabled(ctx.config, widgetName, allSpecs);
      if (compactStyle && selectedValue == "none") {
        selectedValue = "id";
      }

      std::vector<SelectOption> options;
      options.reserve(displaySpec.options.size());
      for (const auto& option : displaySpec.options) {
        if (compactStyle && option.value == "none") {
          continue;
        }
        options.push_back(
            SelectOption{option.value, displaySpec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
        );
      }
      SelectSetting selectSetting{std::move(options), std::move(selectedValue)};
      selectSetting.segmented = displaySpec.segmented;
      return selectSetting;
    }

    SelectSetting labelFontWeightSelectSetting(
        const WidgetSettingSpec& spec, std::string selectedValue, std::string_view fontFamily
    ) {
      std::optional<int> preserveWeight;
      if (!selectedValue.empty()) {
        preserveWeight = static_cast<int>(std::strtol(selectedValue.c_str(), nullptr, 10));
      }

      std::vector<SelectOption> options;
      const auto catalogOptions =
          buildLabelFontWeightSelectOptions(fontFamily, FontWeightSelectKind::WidgetInheritDefault, preserveWeight);
      options.reserve(catalogOptions.size());
      for (const auto& option : catalogOptions) {
        options.push_back(SelectOption{option.value, i18n::tr(option.labelKey)});
      }

      SelectSetting selectSetting{std::move(options), std::move(selectedValue)};
      selectSetting.valueType = spec.integerValue ? SelectValueType::Integer : SelectValueType::String;
      return selectSetting;
    }

    SelectSetting
    sourcedSelectSetting(const BarWidgetEditorContext& ctx, const WidgetSettingSpec& spec, std::string selectedValue) {
      std::vector<SelectOption> options;
      const auto appendUnique = [&](SelectOption option) {
        if (!std::ranges::contains(options, option.value, &SelectOption::value)) {
          options.push_back(std::move(option));
        }
      };
      options.reserve(spec.options.size());
      for (const auto& option : spec.options) {
        appendUnique(
            SelectOption{
                .value = option.value,
                .label = spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey),
            }
        );
      }

      std::vector<SelectOption> sourcedOptions;
      switch (spec.optionSource) {
      case WidgetSettingOptionSource::BatteryDevices:
        sourcedOptions = ctx.batteryDeviceOptions;
        break;
      case WidgetSettingOptionSource::Static:
        break;
      }
      options.reserve(options.size() + sourcedOptions.size());
      for (auto& option : sourcedOptions) {
        appendUnique(std::move(option));
      }

      const auto hasEmptyOption = std::ranges::contains(options, std::string_view{}, &SelectOption::value);
      if (selectedValue.empty() && !hasEmptyOption) {
        selectedValue = settingValueAsString(spec.schema.defaultValue);
      }

      const auto hasSelected = std::ranges::contains(options, selectedValue, &SelectOption::value);
      if (!selectedValue.empty() && !hasSelected) {
        options.push_back(
            SelectOption{
                .value = selectedValue,
                .label = i18n::tr("settings.controls.select.unknown-value", "value", selectedValue),
            }
        );
      }

      return SelectSetting{std::move(options), std::move(selectedValue)};
    }

    void addRawWidgetSettings(
        Flex& panel, std::string_view widgetName, const std::vector<WidgetSettingSpec>& specs,
        std::size_t& visibleSpecs, const BarWidgetEditorContext& ctx
    ) {
      if (!ctx.showAdvanced) {
        return;
      }

      const auto widgetIt = ctx.config.widgets.find(std::string(widgetName));
      if (widgetIt == ctx.config.widgets.end()) {
        return;
      }

      std::unordered_set<std::string> knownKeys;
      knownKeys.reserve(specs.size());
      for (const auto& spec : specs) {
        knownKeys.insert(spec.schema.key);
      }

      std::vector<std::string> rawKeys;
      for (const auto& [key, value] : widgetIt->second.settings) {
        if (knownKeys.contains(key)) {
          continue;
        }
        const auto path = widgetSettingPath(std::string(widgetName), key);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        if (ctx.showOverriddenOnly && !overridden) {
          continue;
        }
        rawKeys.push_back(key);
      }

      if (rawKeys.empty()) {
        return;
      }
      std::ranges::sort(rawKeys);

      panel.addChild(
          ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = 1.0f * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = 0,
              },
              makeLabel(
                  i18n::tr("settings.entities.widget.raw.title"), Style::fontSizeCaption * ctx.scale,
                  colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
              ),
              makeSettingSubtitleLabel(i18n::tr("settings.entities.widget.raw.description"), ctx.scale)
          )
      );

      for (const auto& key : rawKeys) {
        const auto valueIt = widgetIt->second.settings.find(key);
        if (valueIt == widgetIt->second.settings.end()) {
          continue;
        }
        const auto path = widgetSettingPath(std::string(widgetName), key);
        const std::string deleteKey = pathKey(path);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        const bool pendingDelete = ctx.pendingDeleteWidgetSettingPath == deleteKey;

        auto row = ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = 0,
                .minHeight = Style::controlHeightSm * ctx.scale,
            },
            makeLabel(
                key, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
            ),
            ui::spacer(),
            makeLabel(
                settingValueAsDisplayString(valueIt->second), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
            )
        );

        if (overridden) {
          row->addChild(
              ui::button({
                  .text = pendingDelete ? std::optional<std::string>(i18n::tr("settings.entities.widget.raw.delete"))
                                        : std::nullopt,
                  .glyph = "trash",
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = pendingDelete ? ButtonVariant::Default : ButtonVariant::Ghost,
                  .minWidth = Style::controlHeightSm * ctx.scale,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .padding = Style::spaceXs * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [&pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath, deleteKey, path,
                              clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild]() {
                    if (pendingDeleteWidgetSettingPath != deleteKey) {
                      pendingDeleteWidgetSettingPath = deleteKey;
                      requestRebuild();
                      return;
                    }

                    pendingDeleteWidgetSettingPath.clear();
                    clearOverride(path);
                  },
              })
          );
        }

        panel.addChild(std::move(row));
        ++visibleSpecs;
      }
    }

    void addWidgetSettingsPanel(
        Flex& item, std::string widgetName, const std::vector<std::string>& lanePath, const BarWidgetEditorContext& ctx
    ) {
      const auto widgetType = widgetTypeForReference(ctx.config, widgetName);
      if (widgetType.empty()) {
        return;
      }

      const auto widgetIt = ctx.config.widgets.find(widgetName);
      const WidgetConfig* widgetConfig = widgetIt != ctx.config.widgets.end() ? &widgetIt->second : nullptr;
      auto specs = widgetSettingSpecs(
          widgetType, widgetConfig, ctx.config.shell.fontFamily, ctx.supportsTaskbarWorkspaceGrouping
      );
      if (specs.empty()) {
        return;
      }

      auto panel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
      });

      std::size_t visibleSpecs = 0;
      std::string activeGroupKey;
      // Coalesce specs by group so each group header renders once regardless of spec declaration order.
      const auto specOrder = coalesceByGroupKey(specs.size(), [&](std::size_t i) { return specs[i].group; });
      const bool barHorizontal =
          lanePath.size() >= 2 && lanePath[0] == "bar" ? isBarHorizontal(ctx.config, lanePath[1]) : true;
      for (const std::size_t specIndex : specOrder) {
        const auto& spec = specs[specIndex];
        if (!spec.visibleInInspector) {
          continue;
        }
        if (spec.horizontalBarOnly && !barHorizontal) {
          continue;
        }
        if (!isSettingVisible(ctx.config, widgetName, spec, specs)) {
          continue;
        }
        if (spec.advanced && !ctx.showAdvanced) {
          continue;
        }
        const auto path = widgetSettingPath(widgetName, spec.schema.key);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        if (ctx.showOverriddenOnly && !overridden) {
          continue;
        }

        if (spec.group != activeGroupKey) {
          panel->addChild(makeMiniSectionHeader(widgetSettingGroupTitle(spec.group), ctx.scale, visibleSpecs > 0));
          activeGroupKey = spec.group;
        }

        const auto value = widgetSettingValue(ctx.config, widgetName, spec);
        SettingEntry entry{
            .section = SettingsSection::Bar,
            .group = "widget-settings",
            .title = !spec.literalLabel.empty() ? spec.literalLabel
                : spec.labelKey.empty()         ? std::string{}
                                                : i18n::tr(spec.labelKey),
            .subtitle = !spec.literalDescription.empty() ? spec.literalDescription
                : spec.descriptionKey.empty()            ? std::string{}
                                                         : i18n::tr(spec.descriptionKey),
            .path = path,
            .control = TextSetting{},
            .advanced = spec.advanced,
            .searchText = {},
        };

        const auto makeGlyphTextControl = [&ctx, path](std::string currentValue) -> std::unique_ptr<Node> {
          auto textNode = ctx.makeText(currentValue, {}, path);
          return ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceSm * ctx.scale,
              },
              std::move(textNode),
              ui::button({
                  .glyph = "apps",
                  .glyphSize = Style::fontSizeBody * ctx.scale,
                  .variant = ButtonVariant::Default,
                  .minWidth = Style::controlHeight * ctx.scale,
                  .minHeight = Style::controlHeight * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusMd(ctx.scale),
                  .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path,
                              currentValue = std::move(currentValue)]() {
                    GlyphPickerDialogOptions options;
                    if (!currentValue.empty()) {
                      options.initialGlyph = currentValue;
                    }
                    (void)GlyphPickerDialog::open(
                        std::move(options),
                        [setOverride, requestRebuild, path](std::optional<GlyphPickerResult> result) {
                          if (!result.has_value()) {
                            return;
                          }
                          setOverride(path, result->name);
                          if (requestRebuild) {
                            requestRebuild();
                          }
                        }
                    );
                  },
              })
          );
        };

        switch (spec.control) {
        case WidgetControlKind::Bool: {
          std::optional<bool> clearWhenValue;
          if (const auto* defaultBool = std::get_if<bool>(&spec.schema.defaultValue)) {
            clearWhenValue = *defaultBool;
          }
          ctx.makeRow(*panel, entry, ctx.makeToggle(settingValueAsBool(value), path, clearWhenValue));
          break;
        }
        case WidgetControlKind::Int: {
          // A plugin manifest may declare minValue > maxValue; order the range so
          // the clamp, stepper, and slider below all get a valid [min, max].
          const double rawMin = spec.schema.minValue.value_or(0.0);
          const double rawMax = spec.schema.maxValue.value_or(100.0);
          const double minValue = std::min(rawMin, rawMax);
          const double maxValue = std::max(rawMin, rawMax);
          if (spec.stepper) {
            const int minStep = static_cast<int>(std::lround(minValue));
            const int maxStep = static_cast<int>(std::lround(maxValue));
            const int stepValue = static_cast<int>(std::clamp(
                settingValueAsInt(value), static_cast<std::int64_t>(minStep), static_cast<std::int64_t>(maxStep)
            ));
            ctx.makeRow(
                *panel, entry,
                ctx.makeStepper(
                    StepperSetting{
                        .value = stepValue,
                        .minValue = minStep,
                        .maxValue = maxStep,
                        .step = static_cast<int>(std::max(1.0, spec.schema.step.value_or(1.0))),
                        .valueSuffix = spec.valueSuffix,
                    },
                    path
                )
            );
          } else {
            ctx.makeRow(
                *panel, entry,
                ctx.makeSlider(
                    static_cast<double>(settingValueAsInt(value)), minValue, maxValue, spec.schema.step.value_or(1.0),
                    path, true
                )
            );
          }
          break;
        }
        case WidgetControlKind::Double: {
          const double minValue = spec.schema.minValue.value_or(0.0);
          const double maxValue = spec.schema.maxValue.value_or(1.0);
          ctx.makeRow(
              *panel, entry,
              ctx.makeSlider(
                  settingValueAsDouble(value), minValue, maxValue, spec.schema.step.value_or(1.0), path, false
              )
          );
          break;
        }
        case WidgetControlKind::OptionalDouble: {
          ctx.makeRow(
              *panel, entry,
              ctx.makeOptionalStepper(
                  OptionalStepperSetting{
                      .value = widgetSettingOptionalStepperValue(ctx.config, widgetName, spec.schema.key),
                      .minValue = static_cast<int>(std::lround(spec.schema.minValue.value_or(0.0))),
                      .maxValue = static_cast<int>(std::lround(spec.schema.maxValue.value_or(80.0))),
                      .step = static_cast<int>(std::max(1.0, spec.schema.step.value_or(1.0))),
                      .fallbackValue = inheritedCapsuleRadiusForLane(ctx.config, lanePath),
                      .unsetLabel = i18n::tr("common.states.inherit"),
                      .customLabel = i18n::tr("common.states.custom")
                  },
                  path
              )
          );
          break;
        }
        case WidgetControlKind::String: {
          if (spec.schema.key == "custom_image") {
            FileDialogOptions options;
            options.mode = FileDialogMode::Open;
            options.defaultViewMode = FileDialogViewMode::Grid;
            options.title = i18n::tr("settings.widgets.settings.custom-image.dialog-title");
            options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif"};
            options.startDirectory = "/usr/share/icons";
            ctx.makeRow(
                *panel, entry,
                makePathBrowseControl(
                    ctx, path, settingValueAsString(value), "photo", std::move(options), PathBrowseKind::File
                )
            );
          } else {
            ctx.makeRow(*panel, entry, ctx.makeText(settingValueAsString(value), {}, path));
          }
          break;
        }
        case WidgetControlKind::File: {
          FileDialogOptions options;
          options.mode = FileDialogMode::Open;
          options.defaultViewMode = FileDialogViewMode::List;
          options.title = i18n::tr("settings.controls.path-browse.file-title");
          options.extensions = spec.extensions;
          ctx.makeRow(
              *panel, entry,
              makePathBrowseControl(
                  ctx, path, settingValueAsString(value), "file-text", std::move(options), PathBrowseKind::File
              )
          );
          break;
        }
        case WidgetControlKind::Folder: {
          FileDialogOptions options;
          options.mode = FileDialogMode::SelectFolder;
          options.defaultViewMode = FileDialogViewMode::List;
          options.title = i18n::tr("settings.controls.path-browse.folder-title");
          ctx.makeRow(
              *panel, entry,
              makePathBrowseControl(
                  ctx, path, settingValueAsString(value), "folder", std::move(options), PathBrowseKind::Folder
              )
          );
          break;
        }
        case WidgetControlKind::Glyph:
          ctx.makeRow(*panel, entry, makeGlyphTextControl(settingValueAsString(value)));
          break;
        case WidgetControlKind::StringList:
          ctx.makeListBlock(*panel, entry, ListSetting{.items = settingValueAsStringList(value)});
          break;
        case WidgetControlKind::StringMap: {
          const bool customLabels = spec.schema.key == "custom_labels";
          const bool effectsProfileGlyphs = spec.schema.key == "effects_profile_glyphs";
          WidgetSettingStringMap entries;
          if (widgetConfig != nullptr) {
            if (const auto tableIt = widgetConfig->tables.find(spec.schema.key);
                tableIt != widgetConfig->tables.end()) {
              entries = tableIt->second;
            } else if (const auto* defaults = std::get_if<WidgetSettingStringMap>(&spec.schema.defaultValue)) {
              entries = *defaults;
            }
          } else if (const auto* defaults = std::get_if<WidgetSettingStringMap>(&spec.schema.defaultValue)) {
            entries = *defaults;
          }
          ctx.makeStringMapBlock(
              *panel, entry,
              StringMapSetting{
                  .entries = std::move(entries),
                  .suggestedKeys = customLabels ? ctx.keyboardLayoutNames : std::vector<std::string>{},
                  .keyPlaceholder = i18n::tr(
                      customLabels               ? "settings.widgets.map-placeholders.layout-name"
                          : effectsProfileGlyphs ? "settings.widgets.map-placeholders.effects-profile-name"
                                                 : "settings.widgets.map-placeholders.key"
                  ),
                  .valuePlaceholder = i18n::tr(
                      customLabels               ? "settings.widgets.map-placeholders.label"
                          : effectsProfileGlyphs ? "settings.widgets.map-placeholders.glyph-name"
                                                 : "settings.widgets.map-placeholders.value"
                  ),
              }
          );
          break;
        }
        case WidgetControlKind::Select: {
          SelectSetting selectSetting;
          const std::string selectedValue = settingValueAsString(value);
          // Font family uses the filterable search picker (catalogs can hold thousands of families).
          if (spec.schema.key == "font_family" && ctx.makeSearchPicker) {
            std::vector<SelectOption> familyOptions;
            familyOptions.reserve(spec.options.size());
            for (const auto& option : spec.options) {
              familyOptions.push_back(
                  SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
              );
            }
            SearchPickerSetting picker;
            picker.options = std::move(familyOptions);
            picker.selectedValue = selectedValue;
            picker.placeholder = ctx.config.shell.fontFamily;
            picker.emptyText = i18n::tr("ui.controls.search-picker.empty");
            ctx.makeRow(*panel, entry, ctx.makeSearchPicker(picker, entry.title, path));
            break;
          }
          if (spec.optionSource != WidgetSettingOptionSource::Static) {
            selectSetting = sourcedSelectSetting(ctx, spec, selectedValue);
          } else if (spec.schema.key == "font_weight") {
            selectSetting = labelFontWeightSelectSetting(
                spec, widgetLabelFontWeightSelectedValue(ctx.config, widgetName),
                widgetResolvedFontFamily(ctx.config, widgetName)
            );
          } else if (widgetType == "workspaces" && spec.schema.key == "display") {
            selectSetting = workspacesDisplaySelectSetting(ctx, widgetName, spec, specs, selectedValue);
          } else if (widgetType == "workspaces" && spec.schema.key == "style") {
            selectSetting = workspacesStyleSelectSetting(ctx, widgetName, spec, specs, selectedValue);
          } else {
            std::vector<SelectOption> options;
            options.reserve(spec.options.size());
            for (const auto& option : spec.options) {
              options.push_back(
                  SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
              );
            }
            selectSetting = SelectSetting{std::move(options), selectedValue};
          }
          selectSetting.segmented = spec.segmented;
          selectSetting.valueType = spec.integerValue ? SelectValueType::Integer : SelectValueType::String;
          if (const auto* defaultString = std::get_if<std::string>(&spec.schema.defaultValue);
              defaultString != nullptr) {
            selectSetting.clearOnEmpty = defaultString->empty();
          }
          ctx.makeRow(*panel, entry, ctx.makeSelect(selectSetting, path));
          break;
        }
        case WidgetControlKind::ColorSpec: {
          ColorSpecPickerSetting pickerSetting;
          pickerSetting.selectedValue = settingValueAsString(value);
          pickerSetting.allowNone = spec.advanced;
          pickerSetting.allowCustomColor = spec.allowCustomColor;
          ctx.makeRow(*panel, entry, ctx.makeColorSpecPicker(pickerSetting, path));
          break;
        }
        }
        ++visibleSpecs;
      }

      addRawWidgetSettings(*panel, widgetName, specs, visibleSpecs, ctx);

      if (visibleSpecs == 0) {
        panel->addChild(makeLabel(
            i18n::tr("settings.entities.widget.settings.empty"), Style::fontSizeCaption * ctx.scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        ));
      }

      item.addChild(std::move(panel));
    }

    void addInspectorPane(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx) {
      static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};

      if (ctx.editingWidgetName.empty()) {
        return;
      }

      {
        const std::string widgetName = ctx.editingWidgetName;
        const bool guiManaged = isGuiManagedNamedWidgetInstance(ctx, widgetName);

        std::string currentLaneKey;
        std::vector<std::string> currentLanePath;
        std::vector<std::string> currentLaneItems;
        bool currentLaneInherited = false;
        for (const auto laneKey : kLaneKeys) {
          auto p = pathWithLastSegment(laneListPath, std::string(laneKey));
          auto items = barWidgetItemsForPath(ctx.config, p);
          if (std::ranges::contains(items, widgetName)) {
            currentLaneKey = std::string(laneKey);
            currentLanePath = std::move(p);
            currentLaneItems = std::move(items);
            currentLaneInherited = isMonitorWidgetListPath(currentLanePath)
                && !monitorWidgetListHasExplicitValue(ctx.config, currentLanePath);
            break;
          }
        }

        const std::vector<BarCapsuleGroupStyle> inspectorGroups = capsuleGroupsForLanePath(ctx.config, laneListPath);
        std::string capsuleGroup;
        for (const auto& g : inspectorGroups) {
          if (std::ranges::contains(g.members, widgetName)) {
            capsuleGroup = g.id;
            break;
          }
        }
        if (!capsuleGroup.empty()) {
          auto groupRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale, .fillWidth = true});
          groupRow->addChild(
              makeGlyph("stack-2", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::Primary))
          );
          auto hint = makeLabel(
              i18n::tr("settings.entities.widget.group.hint"), Style::fontSizeCaption * ctx.scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          );
          hint->setFlexGrow(1.0f);
          groupRow->addChild(std::move(hint));
          const std::string& editGroupId = capsuleGroup;
          groupRow->addChild(
              ui::button({
                  .text = i18n::tr("settings.entities.widget.group.edit"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [openCapsuleGroupInspector = ctx.openCapsuleGroupInspector, laneListPath, editGroupId]() {
                    if (openCapsuleGroupInspector) {
                      openCapsuleGroupInspector(laneListPath, editGroupId);
                    }
                  },
              })
          );
          body.addChild(std::move(groupRow));
        }

        const bool pendingDelete = ctx.pendingDeleteWidgetName == widgetName;
        const bool renaming = ctx.renamingWidgetName == widgetName;

        if (!currentLaneInherited && !currentLaneKey.empty()) {
          auto actionRow = ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
          });

          actionRow->addChild(ui::spacer());

          for (const auto targetLane : kLaneKeys) {
            if (targetLane == currentLaneKey) {
              continue;
            }
            auto sourceItems = currentLaneItems;
            auto sourcePath = currentLanePath;
            auto targetPath = pathWithLastSegment(laneListPath, std::string(targetLane));
            auto targetItems = barWidgetItemsForPath(ctx.config, targetPath);
            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.inspector.move-to-lane", "lane", laneLabel(targetLane)),
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [setOverrides = ctx.setOverrides, sourceItems, sourcePath, targetItems, targetPath,
                                widgetName]() mutable {
                      auto it = std::ranges::find(sourceItems, widgetName);
                      if (it == sourceItems.end()) {
                        return;
                      }
                      sourceItems.erase(it);
                      targetItems.push_back(widgetName);
                      setOverrides({{sourcePath, sourceItems}, {targetPath, targetItems}});
                    },
                })
            );
          }

          if (guiManaged) {
            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.instance.rename"),
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&renamingWidgetName = ctx.renamingWidgetName,
                                &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName, widgetName,
                                requestRebuild = ctx.requestRebuild]() {
                      renamingWidgetName = widgetName;
                      pendingDeleteWidgetName.clear();
                      requestRebuild();
                    },
                })
            );

            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.instance.delete"),
                    .glyph = "trash",
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .glyphSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                                &renamingWidgetName = ctx.renamingWidgetName, widgetName,
                                requestRebuild = ctx.requestRebuild]() {
                      pendingDeleteWidgetName = widgetName;
                      renamingWidgetName.clear();
                      requestRebuild();
                    },
                })
            );
          }

          body.addChild(std::move(actionRow));
        }

        if (renaming) {
          auto renameRow = ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
          });

          Input* inputPtr = nullptr;
          auto input = ui::input({
              .out = &inputPtr,
              .value = widgetName,
              .placeholder = i18n::tr("settings.entities.widget.instance.id-placeholder"),
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .controlHeight = Style::controlHeightSm * ctx.scale,
              .horizontalPadding = Style::spaceXs * ctx.scale,
              .width = 140.0f * ctx.scale,
              .height = Style::controlHeightSm * ctx.scale,
              .flexGrow = 1.0f,
          });

          auto doRename = [&editingWidgetName = ctx.editingWidgetName, &renamingWidgetName = ctx.renamingWidgetName,
                           config = ctx.config, renameWidgetInstance = ctx.renameWidgetInstance, widgetName,
                           inputPtr](std::string newName) mutable {
            if (!canRenameWidgetInstance(config, widgetName, newName)) {
              inputPtr->setInvalid(true);
              return;
            }
            inputPtr->setInvalid(false);
            auto referenceRenames = widgetReferenceRenameOverrides(config, widgetName, newName);
            renamingWidgetName.clear();
            if (editingWidgetName == widgetName) {
              editingWidgetName = newName;
            }
            renameWidgetInstance(widgetName, std::move(newName), std::move(referenceRenames));
          };

          input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
          input->setOnSubmit([doRename](const std::string& text) mutable { doRename(text); });

          renameRow->addChild(std::move(input));
          renameRow->addChild(
              ui::button({
                  .text = i18n::tr("settings.entities.widget.instance.rename-save"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Default,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [doRename, inputPtr]() mutable { doRename(inputPtr->value()); },
              })
          );
          renameRow->addChild(
              ui::button({
                  .text = i18n::tr("common.actions.cancel"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [&renamingWidgetName = ctx.renamingWidgetName, requestRebuild = ctx.requestRebuild]() {
                    renamingWidgetName.clear();
                    requestRebuild();
                  },
              })
          );
          body.addChild(std::move(renameRow));
        }

        if (pendingDelete) {
          auto confirmPanel = ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = Style::spaceXs * ctx.scale,
                  .padding = Style::spaceSm * ctx.scale,
                  .fill = colorSpecFromRole(ColorRole::Error, 0.10f),
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .border = colorSpecFromRole(ColorRole::Error, 0.5f),
              },
              makeLabel(
                  i18n::tr("settings.entities.widget.instance.delete-confirm-title", "name", widgetName),
                  Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold
              ),
              makeLabel(
                  i18n::tr("settings.entities.widget.instance.delete-confirm-desc"), Style::fontSizeCaption * ctx.scale,
                  colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
              ),
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceSm * ctx.scale,
                  },
                  ui::spacer(),
                  ui::button({
                      .text = i18n::tr("common.actions.cancel"),
                      .fontSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Ghost,
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .paddingV = Style::spaceXs * ctx.scale,
                      .paddingH = Style::spaceSm * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick =
                          [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                           requestRebuild = ctx.requestRebuild]() {
                            pendingDeleteWidgetName.clear();
                            requestRebuild();
                          },
                  }),
                  ui::button({
                      .text = i18n::tr("settings.entities.widget.instance.delete"),
                      .glyph = "trash",
                      .fontSize = Style::fontSizeCaption * ctx.scale,
                      .glyphSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Destructive,
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .paddingV = Style::spaceXs * ctx.scale,
                      .paddingH = Style::spaceSm * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick = [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName, config = ctx.config,
                                  widgetName, clearOverride = ctx.clearOverride, setOverrides = ctx.setOverrides,
                                  closeHostedEditor = ctx.closeHostedEditor]() {
                        pendingDeleteWidgetName.clear();
                        auto referenceRemovals = widgetReferenceRemovalOverrides(config, widgetName);
                        if (!referenceRemovals.empty()) {
                          setOverrides(std::move(referenceRemovals));
                        }
                        clearOverride({"widget", widgetName});
                        if (closeHostedEditor) {
                          closeHostedEditor();
                        }
                      },
                  })
              )
          );
          body.addChild(std::move(confirmPanel));
        }

        addWidgetSettingsPanel(body, widgetName, currentLanePath, ctx);
      }
    }

    // Color picker control (no label) — placed into a standard settings row via ctx.makeRow.
    std::unique_ptr<Node> makeGroupColorControl(
        const BarWidgetEditorContext& ctx, std::string selectedValue, bool allowNone,
        std::function<void(std::optional<ColorSpec>)> onChange
    ) {
      ColorSpecSelectOptions opts;
      opts.selectedValue = std::move(selectedValue);
      opts.allowNone = allowNone;
      opts.allowCustomColor = true;
      opts.fontSize = Style::fontSizeBody * ctx.scale;
      opts.controlHeight = Style::controlHeight * ctx.scale;
      opts.glyphSize = Style::fontSizeBody * ctx.scale;
      opts.width = 190.0f * ctx.scale;
      return makeColorSpecSelect(
          std::move(opts),
          [onChange](std::string value) { onChange(colorSpecFromConfigString(value, "bar.capsule_group.color")); },
          [onChange]() { onChange(std::nullopt); }
      );
    }

    // Slider + editable numeric value field (no label), matching the shell's standard slider control.
    std::unique_ptr<Node> makeGroupSliderControl(
        const BarWidgetEditorContext& ctx, double value, double minV, double maxV, double step, bool integerValue,
        std::function<void(double)> onCommit
    ) {
      Input* valueInputPtr = nullptr;
      auto valueInput = ui::input({
          .out = &valueInputPtr,
          .value = formatSliderValue(value, integerValue),
          .fontSize = Style::fontSizeCaption * ctx.scale,
          .controlHeight = Style::controlHeightSm * ctx.scale,
          .horizontalPadding = Style::spaceXs * ctx.scale,
          .width = 50.0f * ctx.scale,
          .height = Style::controlHeightSm * ctx.scale,
      });

      Slider* sliderPtr = nullptr;
      auto slider = ui::slider({
          .out = &sliderPtr,
          .minValue = minV,
          .maxValue = maxV,
          .step = step,
          .value = value,
          .trackHeight = Style::sliderTrackHeight * ctx.scale,
          .thumbSize = Style::sliderThumbSize * ctx.scale,
          .controlHeight = Style::controlHeight * ctx.scale,
          .width = Style::sliderDefaultWidth * ctx.scale,
          .height = Style::controlHeight * ctx.scale,
          .onValueChanged = [valueInputPtr, integerValue](double next) {
            valueInputPtr->setInvalid(false);
            valueInputPtr->setValue(formatSliderValue(next, integerValue));
          },
      });
      valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), integerValue));
      slider->setOnDragEnd([sliderPtr, onCommit]() { onCommit(sliderPtr->value()); });

      const auto commitInputText = [sliderPtr, valueInputPtr, minV, maxV, integerValue,
                                    onCommit](const std::string& text) {
        const auto parsed = parseDoubleInput(text);
        if (!parsed.has_value() || *parsed < minV || *parsed > maxV) {
          valueInputPtr->setInvalid(true);
          return false;
        }
        valueInputPtr->setInvalid(false);
        sliderPtr->setValue(*parsed);
        valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), integerValue));
        onCommit(sliderPtr->value());
        return true;
      };
      valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
      valueInput->setOnSubmit([commitInputText](const std::string& text) { (void)commitInputText(text); });
      valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { (void)commitInputText(valueInputPtr->value()); });

      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      wrap->addChild(std::move(slider));
      wrap->addChild(std::move(valueInput));
      return wrap;
    }

    // Radius Auto | Custom segmented control + stepper (no label).
    std::unique_ptr<Node> makeGroupRadiusControl(
        const BarWidgetEditorContext& ctx, std::optional<float> radius,
        std::function<void(std::optional<float>)> onChange
    ) {
      const int radiusValue = static_cast<int>(std::lround(std::clamp(radius.value_or(12.0f), 0.0f, 80.0f)));
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      wrap->addChild(
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      {.label = i18n::tr("common.states.auto")},
                      {.label = i18n::tr("common.states.custom")},
                  },
              .selectedIndex = static_cast<std::size_t>(radius.has_value() ? 1 : 0),
              .scale = ctx.scale,
              .onChange = [onChange, radiusValue](std::size_t index) {
                onChange(index == 0 ? std::optional<float>{} : std::optional<float>{static_cast<float>(radiusValue)});
              },
          })
      );
      wrap->addChild(
          ui::stepper({
              .minValue = 0,
              .maxValue = 80,
              .step = 1,
              .value = radiusValue,
              .enabled = radius.has_value(),
              .scale = ctx.scale,
              .onValueCommitted = [onChange](int v) { onChange(std::optional<float>{static_cast<float>(v)}); },
          })
      );
      return wrap;
    }

    void addCapsuleGroupInspector(
        Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx
    ) {
      if (ctx.editingCapsuleGroupId.empty() || laneListPath.size() < 2 || laneListPath[0] != "bar") {
        return;
      }
      const std::string groupId = ctx.editingCapsuleGroupId;
      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
      if (groupPath.empty()) {
        return;
      }
      const std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(ctx.config, laneListPath);
      const BarCapsuleGroupStyle* stylePtr = findCapsuleGroupStyle(groups, groupId);
      if (stylePtr == nullptr) {
        if (ctx.closeHostedEditor) {
          ctx.closeHostedEditor();
        }
        return;
      }
      const BarCapsuleGroupStyle style = *stylePtr;
      const std::size_t memberCount = stylePtr->members.size();

      body.addChild(makeSettingSubtitleLabel(
          i18n::tr("settings.entities.widget.group.members", "count", std::to_string(memberCount)), ctx.scale
      ));

      // Commits a mutated copy of the group style vector.
      const auto mutateGroup = [setOverride = ctx.setOverride, groups, groupPath,
                                groupId](const std::function<void(BarCapsuleGroupStyle&)>& fn) {
        std::vector<BarCapsuleGroupStyle> updated = groups;
        for (auto& g : updated) {
          if (g.id == groupId) {
            fn(g);
            break;
          }
        }
        setOverride(groupPath, updated);
      };

      // Controls use the standard settings row (ctx.makeRow), matching the per-widget settings editor.
      auto panel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
      });
      Flex* panelPtr = panel.get();

      const auto groupEntry = [&](std::string_view field) {
        const std::string base = std::string("settings.entities.widget.group.") + std::string(field);
        std::vector<std::string> fieldPath = groupPath;
        fieldPath.push_back(groupId);
        fieldPath.emplace_back(field);
        return SettingEntry{
            .section = SettingsSection::Bar,
            .group = "capsule-group",
            .title = i18n::tr(base),
            .subtitle = i18n::tr(base + "-description"),
            .path = std::move(fieldPath),
            .control = {},
            .searchText = {},
        };
      };

      panel->addChild(makeMiniSectionHeader(i18n::tr("settings.entities.widget.group.style"), ctx.scale, false));

      ctx.makeRow(
          *panelPtr, groupEntry("fill"),
          makeGroupColorControl(
              ctx, colorSpecConfigValue(style.fill), false, [mutateGroup](std::optional<ColorSpec> c) {
                if (c.has_value()) {
                  mutateGroup([&](BarCapsuleGroupStyle& g) { g.fill = *c; });
                }
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("border"),
          makeGroupColorControl(
              ctx, optionalColorSpecConfigValue(style.border), true, [mutateGroup](std::optional<ColorSpec> c) {
                mutateGroup([&](BarCapsuleGroupStyle& g) {
                  g.borderSpecified = true;
                  g.border = c;
                });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("foreground"),
          makeGroupColorControl(
              ctx, optionalColorSpecConfigValue(style.foreground), true, [mutateGroup](std::optional<ColorSpec> c) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.foreground = c; });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("padding"),
          makeGroupSliderControl(
              ctx, static_cast<double>(style.padding), 0.0, 48.0, 1.0, true, [mutateGroup](double v) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.padding = static_cast<float>(v); });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("opacity"),
          makeGroupSliderControl(
              ctx, static_cast<double>(style.opacity), 0.0, 1.0, 0.05, false, [mutateGroup](double v) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.opacity = static_cast<float>(v); });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("radius"),
          makeGroupRadiusControl(ctx, style.radius, [mutateGroup](std::optional<float> r) {
            mutateGroup([&](BarCapsuleGroupStyle& g) { g.radius = r; });
          })
      );

      body.addChild(std::move(panel));
      body.addChild(
          ui::button({
              .text = i18n::tr("settings.entities.widget.group.ungroup"),
              .glyph = "stack-pop",
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Default,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .onClick = [setOverrides = ctx.setOverrides, groupId, groupPath, laneListPath, config = &ctx.config,
                          closeHostedEditor = ctx.closeHostedEditor]() {
                std::vector<BarCapsuleGroupStyle> currentGroups = capsuleGroupsForLanePath(*config, laneListPath);
                const BarCapsuleGroupStyle* g = findCapsuleGroupStyle(currentGroups, groupId);
                if (g == nullptr) {
                  if (closeHostedEditor) {
                    closeHostedEditor();
                  }
                  return;
                }
                if (groupPath.empty()) {
                  return;
                }
                const std::vector<std::string> members = g->members;
                std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
                // Replace the group token with its members in whichever lane holds it.
                const std::string token = makeCapsuleGroupToken(groupId);
                for (const auto laneKey : {"start", "center", "end"}) {
                  std::vector<std::string> lanePath = pathWithLastSegment(laneListPath, laneKey);
                  std::vector<std::string> lane = barWidgetItemsForPath(*config, lanePath);
                  const auto it = std::ranges::find(lane, token);
                  if (it == lane.end()) {
                    continue;
                  }
                  const std::size_t pos = static_cast<std::size_t>(it - lane.begin());
                  lane.erase(lane.begin() + static_cast<std::ptrdiff_t>(pos));
                  lane.insert(lane.begin() + static_cast<std::ptrdiff_t>(pos), members.begin(), members.end());
                  batch.emplace_back(lanePath, lane);
                }
                std::vector<BarCapsuleGroupStyle> remaining;
                for (const auto& gx : currentGroups) {
                  if (gx.id != groupId) {
                    remaining.push_back(gx);
                  }
                }
                batch.emplace_back(groupPath, remaining);
                setOverrides(std::move(batch));
                if (closeHostedEditor) {
                  closeHostedEditor();
                }
              },
          })
      );
    }

    struct LaneGroupPlan {
      bool groupable = false;
      std::string laneKey;
      std::size_t firstIndex = 0;
      std::vector<std::string> members; // contiguous selection in lane order
    };

    // Selection tokens are "<laneKey>#<index>". Grouping needs ≥2 selected widgets in one lane that are
    // adjacent and none of which is already a group token.
    LaneGroupPlan computeLaneGroupPlan(const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
      LaneGroupPlan plan;
      const auto& selection = ctx.selectedLaneWidgets;
      if (selection.size() < 2) {
        return plan;
      }
      std::string laneKey;
      std::vector<std::size_t> indices;
      for (const auto& token : selection) {
        const auto hash = token.find('#');
        if (hash == std::string::npos) {
          return plan;
        }
        const std::string key = token.substr(0, hash);
        if (laneKey.empty()) {
          laneKey = key;
        } else if (laneKey != key) {
          return plan; // selection spans multiple lanes
        }
        indices.push_back(static_cast<std::size_t>(std::strtoul(token.c_str() + hash + 1, nullptr, 10)));
      }
      std::ranges::sort(indices);
      for (std::size_t k = 1; k < indices.size(); ++k) {
        if (indices[k] != indices[k - 1] + 1) {
          return plan; // not contiguous
        }
      }

      const std::vector<std::string> items =
          barWidgetItemsForPath(ctx.config, pathWithLastSegment(entry.path, laneKey));
      for (const auto idx : indices) {
        if (idx >= items.size() || isCapsuleGroupToken(items[idx])) {
          return plan;
        }
        plan.members.push_back(items[idx]);
      }
      plan.groupable = true;
      plan.laneKey = laneKey;
      plan.firstIndex = indices.front();
      return plan;
    }

    void addLaneSelectionToolbar(Flex& block, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
      const LaneGroupPlan plan = computeLaneGroupPlan(entry, ctx);

      auto toolbar = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * ctx.scale,
          .paddingV = Style::spaceXs * ctx.scale,
          .paddingH = Style::spaceSm * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::Primary, 0.12f),
          .radius = Style::scaledRadiusSm(ctx.scale),
          .fillWidth = true,
      });
      auto label = makeLabel(
          i18n::tr("settings.entities.widget.group.selected", "count", std::to_string(ctx.selectedLaneWidgets.size())),
          Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      );
      label->setFlexGrow(1.0f);
      toolbar->addChild(std::move(label));

      if (plan.groupable) {
        toolbar->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.group.action"),
                .glyph = "stack-2",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Primary,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [setOverrides = ctx.setOverrides, config = &ctx.config, laneKey = plan.laneKey,
                            firstIndex = plan.firstIndex, members = plan.members, laneListPath = entry.path,
                            &selectedLaneWidgets = ctx.selectedLaneWidgets,
                            openCapsuleGroupInspector = ctx.openCapsuleGroupInspector]() {
                  const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
                  if (groupPath.empty()) {
                    return;
                  }
                  std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, laneListPath);
                  const std::string newId = nextCapsuleGroupId(groups);
                  BarCapsuleGroupStyle newGroup = seedCapsuleGroupStyle(*config, laneListPath, newId);
                  newGroup.members = members;
                  groups.push_back(std::move(newGroup));

                  // Replace the contiguous selected run with a single group token.
                  std::vector<std::string> lanePath = pathWithLastSegment(laneListPath, laneKey);
                  std::vector<std::string> lane = barWidgetItemsForPath(*config, lanePath);
                  const std::size_t count = std::min(members.size(), lane.size() - std::min(firstIndex, lane.size()));
                  if (firstIndex <= lane.size()) {
                    lane.erase(
                        lane.begin() + static_cast<std::ptrdiff_t>(firstIndex),
                        lane.begin() + static_cast<std::ptrdiff_t>(firstIndex + count)
                    );
                    lane.insert(lane.begin() + static_cast<std::ptrdiff_t>(firstIndex), makeCapsuleGroupToken(newId));
                  }

                  selectedLaneWidgets.clear();
                  setOverrides({{lanePath, lane}, {groupPath, groups}});
                  if (openCapsuleGroupInspector) {
                    openCapsuleGroupInspector(laneListPath, newId);
                  }
                },
            })
        );
      }
      toolbar->addChild(
          ui::button({
              .text = i18n::tr("settings.entities.widget.group.clear"),
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, requestRebuild = ctx.requestRebuild]() {
                selectedLaneWidgets.clear();
                requestRebuild();
              },
          })
      );
      block.addChild(std::move(toolbar));
    }

  } // namespace

  bool isBarWidgetListPath(const std::vector<std::string>& path) {
    if (path.size() < 3 || path.front() != "bar") {
      return false;
    }
    const auto& key = path.back();
    return key == "start" || key == "center" || key == "end";
  }

  bool isFirstBarWidgetListPath(const std::vector<std::string>& path) {
    return isBarWidgetListPath(path) && path.back() == "start";
  }

  void buildWidgetInspectorBody(
      Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx
  ) {
    addInspectorPane(body, laneListPath, ctx);
  }

  void
  buildCapsuleGroupBody(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx) {
    addCapsuleGroupInspector(body, laneListPath, ctx);
  }

  void addBarWidgetLaneEditor(Flex& section, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
    if (!isFirstBarWidgetListPath(entry.path)) {
      return;
    }

    auto block = ui::column(
        {
            .align = FlexAlign::Stretch,
            .gap = Style::spaceSm * ctx.scale,
            .paddingV = 2.0f * ctx.scale,
            .paddingH = 0,
        },
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * ctx.scale,
            },
            makeLabel(
                i18n::tr("settings.entities.widget.editor.title"), Style::fontSizeBody * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurface), FontWeight::Normal
            )
        )
    );

    block->addChild(makeSettingSubtitleLabel(i18n::tr("settings.entities.widget.editor.description"), ctx.scale));

    static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};

    // Selection toolbar: Group adjacent selected widgets, or clear the current selection.
    if (!ctx.selectedLaneWidgets.empty()) {
      addLaneSelectionToolbar(*block, entry, ctx);
    }

    auto lanes = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * ctx.scale,
        .fillWidth = true,
    });

    auto zones = std::make_shared<std::vector<DropZone>>();

    // Shared compact icon-button footprint for lane cards and group headers.
    const float iconSize = Style::controlHeightSm * 0.84f * ctx.scale;
    const float iconPad = 2.0f * ctx.scale;
    const float rowGap = 2.0f * ctx.scale;

    // Wires a drag handle so its card can be dragged between any registered zone (lane or group).
    auto wireDrag = [&ctx, zones, laneListPath = entry.path](
                        Button& handle, Button* handlePtr, Flex* cardPtr, std::size_t homeZoneIndex,
                        std::size_t itemIndex
                    ) {
      auto dragState = std::make_shared<LaneWidgetDragState>();
      handle.setOnPress([dragState, cardPtr, zones, config = &ctx.config, laneListPath, setOverride = ctx.setOverride,
                         setOverrides = ctx.setOverrides, homeZoneIndex,
                         itemIndex](float localX, float localY, bool pressed) {
        const auto clearHighlight = [&]() {
          if (dragState->highlightZoneIndex.has_value() && dragState->highlightItemIndex.has_value()) {
            setCardCombineHighlight(*zones, *dragState->highlightZoneIndex, *dragState->highlightItemIndex, false);
          }
          dragState->highlightZoneIndex = std::nullopt;
          dragState->highlightItemIndex = std::nullopt;
        };
        if (pressed) {
          dragState->active = true;
          dragState->moved = false;
          dragState->startLocalX = localX;
          dragState->startLocalY = localY;
          dragState->lastLocalX = localX;
          dragState->lastLocalY = localY;
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          dragState->combineZoneIndex = std::nullopt;
          dragState->combineItemIndex = std::nullopt;
          clearHighlight();
          cardPtr->setOpacity(0.72f);
          hideDropIndicators(*zones);
          return;
        }
        if (!dragState->active) {
          return;
        }
        dragState->active = false;
        cardPtr->setOpacity(1.0f);
        clearHighlight();
        hideDropIndicators(*zones);
        if (!dragState->moved) {
          return;
        }
        if (dragState->combineZoneIndex.has_value() && dragState->combineItemIndex.has_value()) {
          createGroupByCombine(
              *config, *zones, homeZoneIndex, itemIndex, *dragState->combineZoneIndex, *dragState->combineItemIndex,
              setOverrides
          );
          return;
        }
        if (!dragState->targetZoneIndex.has_value() || !dragState->targetInsertionIndex.has_value()) {
          return;
        }
        performZoneMove(
            *config, laneListPath, *zones, homeZoneIndex, itemIndex, *dragState->targetZoneIndex,
            *dragState->targetInsertionIndex, setOverride, setOverrides
        );
      });
      handle.setOnPointerMotion([dragState, handlePtr, zones, homeZoneIndex, itemIndex,
                                 scale = ctx.scale](float localX, float localY) {
        if (!dragState->active) {
          return;
        }
        dragState->lastLocalX = localX;
        dragState->lastLocalY = localY;
        if (std::hypot(localX - dragState->startLocalX, localY - dragState->startLocalY)
            >= Style::dragStartThreshold * scale) {
          dragState->moved = true;
        }
        if (!dragState->moved) {
          return;
        }
        const auto clearHighlight = [&]() {
          if (dragState->highlightZoneIndex.has_value() && dragState->highlightItemIndex.has_value()) {
            setCardCombineHighlight(*zones, *dragState->highlightZoneIndex, *dragState->highlightItemIndex, false);
          }
          dragState->highlightZoneIndex = std::nullopt;
          dragState->highlightItemIndex = std::nullopt;
        };
        const auto clear = [&]() {
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          dragState->combineZoneIndex = std::nullopt;
          dragState->combineItemIndex = std::nullopt;
          clearHighlight();
          hideDropIndicators(*zones);
        };
        float absX = 0.0f;
        float absY = 0.0f;
        Node::absolutePosition(handlePtr, absX, absY);
        const float sceneX = absX + localX;
        const float sceneY = absY + localY;
        const auto targetZone = zoneAtScenePoint(*zones, sceneX, sceneY);
        if (!targetZone.has_value() || *targetZone >= zones->size()) {
          clear();
          return;
        }
        const DropZone& zone = (*zones)[*targetZone];
        if (zone.itemNodes == nullptr || zone.container == nullptr || zone.indicator == nullptr) {
          clear();
          return;
        }

        // Combine: a loose widget dropped onto the middle of another loose widget forms a new group.
        const bool draggedIsLooseWidget = homeZoneIndex < zones->size()
            && !(*zones)[homeZoneIndex].isGroup
            && itemIndex < (*zones)[homeZoneIndex].items.size()
            && !isCapsuleGroupToken((*zones)[homeZoneIndex].items[itemIndex]);
        if (!zone.isGroup && draggedIsLooseWidget) {
          if (const auto hovered = hoveredItemBand(sceneY, *zone.itemNodes); hovered.has_value()) {
            const std::size_t hoveredIdx = hovered->first;
            const bool onMiddle = hovered->second;
            const bool sameItem = *targetZone == homeZoneIndex && hoveredIdx == itemIndex;
            const bool hoveredIsWidget = hoveredIdx < zone.items.size() && !isCapsuleGroupToken(zone.items[hoveredIdx]);
            if (onMiddle && hoveredIsWidget && !sameItem) {
              if (dragState->highlightZoneIndex != *targetZone || dragState->highlightItemIndex != hoveredIdx) {
                clearHighlight();
                setCardCombineHighlight(*zones, *targetZone, hoveredIdx, true);
                dragState->highlightZoneIndex = targetZone;
                dragState->highlightItemIndex = hoveredIdx;
              }
              dragState->combineZoneIndex = targetZone;
              dragState->combineItemIndex = hoveredIdx;
              dragState->targetZoneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              hideDropIndicators(*zones);
              return;
            }
          }
        }

        // Insertion (between items / into a group).
        clearHighlight();
        dragState->combineZoneIndex = std::nullopt;
        dragState->combineItemIndex = std::nullopt;
        const std::size_t insertion = insertionIndexForSceneY(sceneY, *zone.itemNodes);
        if (insertionWouldNotMove(homeZoneIndex, *targetZone, itemIndex, insertion)) {
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          hideDropIndicators(*zones);
          return;
        }
        dragState->targetZoneIndex = targetZone;
        dragState->targetInsertionIndex = insertion;
        hideDropIndicators(*zones);
        updateDropIndicator(*zone.indicator, *zone.container, *zone.itemNodes, insertion, scale);
      });
    };

    // Builds a draggable widget card (used for both loose lane widgets and group members).
    auto makeWidgetCard = [&ctx, &wireDrag, &entry, iconSize, iconPad, rowGap](
                              const std::string& name, std::size_t homeZoneIndex, std::size_t itemIndex, bool inherited,
                              std::string_view removeGlyph, std::function<void()> removeAction, bool selectable,
                              bool isSelected, std::function<void()> toggleSelect
                          ) -> std::unique_ptr<Flex> {
      const auto info = widgetReferenceInfo(ctx.config, name, false);
      auto card = ui::column({
          .align = FlexAlign::Stretch,
          .paddingV = 3.0f * ctx.scale,
          .paddingH = Style::spaceXs * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::Surface, 0.72f),
          .radius = Style::scaledRadiusSm(ctx.scale),
          .border = isSelected ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec(),
          .borderWidth = Style::borderWidth,
      });
      auto* cardPtr = card.get();

      // Single compact row: [checkbox] [drag] title… [kind glyph] [settings] [remove].
      auto row = ui::row({.align = FlexAlign::Center, .gap = rowGap});
      if (selectable) {
        row->addChild(
            ui::button({
                .glyph = isSelected ? "checkbox" : "square",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = isSelected ? ButtonVariant::Default : ButtonVariant::Ghost,
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = std::move(toggleSelect),
            })
        );
      }
      if (!inherited) {
        Button* dragBtnPtr = nullptr;
        auto dragBtn = ui::button({
            .out = &dragBtnPtr,
            .glyph = "menu-2",
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .variant = ButtonVariant::Ghost,
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = iconPad,
            .radius = Style::scaledRadiusSm(ctx.scale),
            .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
        });
        wireDrag(*dragBtn, dragBtnPtr, cardPtr, homeZoneIndex, itemIndex);
        row->addChild(std::move(dragBtn));
      }
      row->addChild(
          makeGlyph(widgetBadgeGlyph(info.kind), Style::fontSizeCaption * ctx.scale, widgetBadgeGlyphColor(info.kind))
      );
      {
        auto titleLabel = makeLabel(
            info.title, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
            FontWeight::SemiBold
        );
        titleLabel->setMaxLines(1);
        titleLabel->setFlexGrow(1.0f);
        row->addChild(std::move(titleLabel));
      }
      if (!widgetTypeForReference(ctx.config, name).empty()) {
        row->addChild(
            ui::button({
                .glyph = "settings",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [openWidgetInspector = ctx.openWidgetInspector, laneListPath = entry.path, name]() {
                  if (openWidgetInspector) {
                    openWidgetInspector(laneListPath, name);
                  }
                },
            })
        );
      }
      {
        bool widgetEnabled = true;
        if (auto it = ctx.config.widgets.find(name); it != ctx.config.widgets.end()) {
          widgetEnabled = it->second.getBool("enabled", true);
        }
        row->addChild(
            ui::button({
                .glyph = widgetEnabled ? "eye" : "eye-off",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = widgetEnabled ? i18n::tr("settings.entities.widget.group.hide-widget")
                                         : i18n::tr("settings.entities.widget.group.show-widget"),
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .opacity = widgetEnabled ? 1.0f : 0.38f,
                .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, name, widgetEnabled]() {
                  setOverride({"widget", name, "enabled"}, !widgetEnabled);
                  if (requestRebuild) {
                    requestRebuild();
                  }
                },
            })
        );
      }
      if (!inherited && removeAction) {
        row->addChild(
            ui::button({
                .glyph = std::string(removeGlyph),
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = std::move(removeAction),
            })
        );
      }
      card->addChild(std::move(row));
      return card;
    };

    for (const auto laneKey : kLaneKeys) {
      auto lanePath = pathWithLastSegment(entry.path, std::string(laneKey));
      const auto laneItems = barWidgetItemsForPath(ctx.config, lanePath);
      const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(lanePath);
      const bool hasGuiOverride = ctx.configService != nullptr && ctx.configService->hasOverride(lanePath);
      const bool monitorLaneExplicit = monitorWidgetListHasExplicitValue(ctx.config, lanePath);
      const bool inherited = isMonitorWidgetListPath(lanePath) && !monitorLaneExplicit;

      auto lane = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
          .padding = Style::spaceSm * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f),
          .radius = Style::scaledRadiusMd(ctx.scale),
          .border = colorSpecFromRole(ColorRole::Outline),
          .minWidth = 160.0f * ctx.scale,
          .flexGrow = 1.0f,
      });
      auto* lanePtr = lane.get();

      auto dropIndicator = ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = std::max(1.0f, 1.5f * ctx.scale),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(10); },
      });
      auto* dropIndicatorPtr = dropIndicator.get();
      lane->addChild(std::move(dropIndicator));

      auto laneItemNodes = std::make_shared<std::vector<Flex*>>();
      laneItemNodes->reserve(laneItems.size());
      const std::size_t laneZoneIndex = zones->size();
      zones->push_back(
          DropZone{
              .isGroup = false,
              .lanePath = lanePath,
              .groupId = {},
              .items = laneItems,
              .container = lanePtr,
              .indicator = dropIndicatorPtr,
              .itemNodes = laneItemNodes,
          }
      );

      auto laneHeader = ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
              // Fixed height so lanes with an Override badge / Reset button line up with plain ones.
              .minHeight = Style::controlHeightSm * ctx.scale,
          },
          makeLabel(
              laneLabel(laneKey), Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
              FontWeight::Bold
          )
      );
      if (overridden) {
        laneHeader->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .paddingV = 0,
                    .paddingH = Style::spaceXs * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::Primary, 0.15f),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                },
                makeLabel(
                    i18n::tr("settings.badges.override"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::Primary), FontWeight::Bold
                )
            )
        );
      }
      if (inherited) {
        laneHeader->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .paddingV = 0,
                    .paddingH = Style::spaceXs * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.14f),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                },
                makeLabel(
                    i18n::tr("settings.badges.inherited"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Bold
                )
            )
        );
      }
      laneHeader->addChild(ui::spacer());
      if (inherited) {
        const auto& items = laneItems;
        const auto& path = lanePath;
        laneHeader->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.lanes.customize"),
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [setOverride = ctx.setOverride, items, path]() { setOverride(path, items); },
            })
        );
      }
      if (overridden || (monitorLaneExplicit && hasGuiOverride)) {
        laneHeader->addChild(ctx.makeResetButton(lanePath));
      }
      lane->addChild(std::move(laneHeader));

      const std::vector<BarCapsuleGroupStyle> laneGroups = capsuleGroupsForLanePath(ctx.config, lanePath);
      for (std::size_t i = 0; i < laneItems.size(); ++i) {
        const std::string& entryName = laneItems[i];

        // Group token → render a container holding its members; members are dragged in/out of it.
        if (isCapsuleGroupToken(entryName)) {
          const std::string gid = capsuleGroupTokenId(entryName);
          const BarCapsuleGroupStyle* group = findCapsuleGroupStyle(laneGroups, gid);
          if (group == nullptr) {
            auto orphan = ui::column({
                .align = FlexAlign::Center,
                .gap = Style::spaceXs * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .border = colorSpecFromRole(ColorRole::Error, 0.5f),
            });
            orphan->addChild(makeLabel(
                i18n::tr("settings.entities.widget.group.orphan"), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
            ));
            if (!inherited) {
              orphan->addChild(
                  ui::button({
                      .glyph = "close",
                      .glyphSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Ghost,
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .padding = Style::spaceXs * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick = [setOverride = ctx.setOverride, items = laneItems, lanePath, i]() mutable {
                        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                        setOverride(lanePath, items);
                      },
                  })
              );
            }
            laneItemNodes->push_back(orphan.get());
            lane->addChild(std::move(orphan));
            continue;
          }

          // Tint the container by the group's own fill so groups with different colors are distinguishable.
          // A color meant to blend into surfaces (e.g. surface_variant) can't separate the box from the lane,
          // so fall back to a neutral border + slight surface lift when the fill is too close to the lane.
          const Color groupFillColor = resolveColorSpec(group->fill);
          const Color laneBgColor = colorForRole(ColorRole::SurfaceVariant);
          const float dr = groupFillColor.r - laneBgColor.r;
          const float dg = groupFillColor.g - laneBgColor.g;
          const float db = groupFillColor.b - laneBgColor.b;
          const bool fillDistinct = std::sqrt(dr * dr + dg * dg + db * db) >= 0.15f;
          ColorSpec groupFillTint;
          ColorSpec groupBorder;
          if (fillDistinct) {
            groupFillTint = group->fill;
            groupFillTint.alpha *= 0.15f;
            groupBorder = group->fill; // full opacity
          } else {
            groupFillTint = colorSpecFromRole(ColorRole::OnSurface, 0.06f); // slight neutral lift
            groupBorder = colorSpecFromRole(ColorRole::Outline);            // full opacity
          }
          auto container = ui::column({
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .fill = groupFillTint,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .border = groupBorder,
              .opacity = group->enabled ? 1.0f : 0.45f,
          });
          auto* containerPtr = container.get();

          auto groupIndicator = ui::box({
              .fill = colorSpecFromRole(ColorRole::Primary),
              .radius = std::max(1.0f, 1.5f * ctx.scale),
              .visible = false,
              .participatesInLayout = false,
              .configure = [](Box& box) { box.setZIndex(10); },
          });
          auto* groupIndicatorPtr = groupIndicator.get();
          container->addChild(std::move(groupIndicator));

          auto groupHeader = ui::row({.align = FlexAlign::Center, .gap = rowGap});
          groupHeader->addChild(
              ui::box({
                  .fill = group->fill,
                  .radius = std::max(1.0f, 2.0f * ctx.scale),
                  .width = Style::fontSizeCaption * ctx.scale,
                  .height = Style::fontSizeCaption * ctx.scale,
                  .configure = [](Box& box) {
                    box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                  },
              })
          );
          {
            auto groupLabel = makeLabel(
                i18n::tr("settings.entities.widget.group.title"), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurface), FontWeight::SemiBold
            );
            groupLabel->setFlexGrow(1.0f);
            groupHeader->addChild(std::move(groupLabel));
          }
          groupHeader->addChild(
              ui::button({
                  .glyph = group->enabled ? "eye" : "eye-off",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .tooltip = group->enabled ? i18n::tr("settings.entities.widget.group.hide")
                                            : i18n::tr("settings.entities.widget.group.show"),
                  .minWidth = iconSize,
                  .minHeight = iconSize,
                  .padding = iconPad,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .opacity = group->enabled ? 1.0f : 0.38f,
                  .onClick = [setOverrides = ctx.setOverrides, groups = laneGroups, lanePathCopy = lanePath, gid,
                              requestRebuild = ctx.requestRebuild]() {
                    std::vector<BarCapsuleGroupStyle> updated = groups;
                    for (auto& g : updated) {
                      if (g.id == gid) {
                        g.enabled = !g.enabled;
                        break;
                      }
                    }
                    const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePathCopy);
                    if (!groupPath.empty()) {
                      setOverrides({{groupPath, updated}});
                      if (requestRebuild) {
                        requestRebuild();
                      }
                    }
                  },
              })
          );
          groupHeader->addChild(
              ui::button({
                  .glyph = "settings",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minWidth = iconSize,
                  .minHeight = iconSize,
                  .padding = iconPad,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [openCapsuleGroupInspector = ctx.openCapsuleGroupInspector, laneListPath = entry.path,
                              gid]() {
                    if (openCapsuleGroupInspector) {
                      openCapsuleGroupInspector(laneListPath, gid);
                    }
                  },
              })
          );
          if (!inherited) {
            groupHeader->addChild(
                ui::button({
                    .glyph = "stack-pop",
                    .glyphSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minWidth = iconSize,
                    .minHeight = iconSize,
                    .padding = iconPad,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [config = &ctx.config, lanePath, gid, setOverrides = ctx.setOverrides]() {
                      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, lanePath);
                      const BarCapsuleGroupStyle* g = findCapsuleGroupStyle(groups, gid);
                      if (g == nullptr) {
                        return;
                      }
                      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePath);
                      if (groupPath.empty()) {
                        return;
                      }
                      const std::vector<std::string> members = g->members;
                      std::vector<std::string> laneEntries = barWidgetItemsForPath(*config, lanePath);
                      const std::string token = makeCapsuleGroupToken(gid);
                      const auto it = std::ranges::find(laneEntries, token);
                      if (it != laneEntries.end()) {
                        const std::size_t pos = static_cast<std::size_t>(it - laneEntries.begin());
                        laneEntries.erase(laneEntries.begin() + static_cast<std::ptrdiff_t>(pos));
                        laneEntries.insert(
                            laneEntries.begin() + static_cast<std::ptrdiff_t>(pos), members.begin(), members.end()
                        );
                      }
                      std::vector<BarCapsuleGroupStyle> remaining;
                      for (const auto& x : groups) {
                        if (x.id != gid) {
                          remaining.push_back(x);
                        }
                      }
                      setOverrides({{lanePath, laneEntries}, {groupPath, remaining}});
                    },
                })
            );
            Button* groupDragPtr = nullptr;
            auto groupDrag = ui::button({
                .out = &groupDragPtr,
                .glyph = "menu-2",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
            });
            wireDrag(*groupDrag, groupDragPtr, containerPtr, laneZoneIndex, i);
            groupHeader->addChild(std::move(groupDrag));
          }
          container->addChild(std::move(groupHeader));

          auto groupItemNodes = std::make_shared<std::vector<Flex*>>();
          const std::size_t groupZoneIndex = zones->size();
          zones->push_back(
              DropZone{
                  .isGroup = true,
                  .lanePath = {},
                  .groupId = gid,
                  .items = group->members,
                  .container = containerPtr,
                  .indicator = groupIndicatorPtr,
                  .itemNodes = groupItemNodes,
              }
          );

          for (std::size_t m = 0; m < group->members.size(); ++m) {
            std::function<void()> eject;
            if (!inherited) {
              eject = [config = &ctx.config, lanePath, gid, m, setOverrides = ctx.setOverrides]() {
                const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePath);
                if (groupPath.empty()) {
                  return;
                }
                std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, lanePath);
                std::string ejected;
                bool emptyNow = false;
                for (auto& g : groups) {
                  if (g.id == gid) {
                    if (m < g.members.size()) {
                      ejected = g.members[m];
                      g.members.erase(g.members.begin() + static_cast<std::ptrdiff_t>(m));
                    }
                    emptyNow = g.members.empty();
                    break;
                  }
                }
                if (ejected.empty()) {
                  return;
                }
                std::vector<std::string> laneEntries = barWidgetItemsForPath(*config, lanePath);
                const std::string token = makeCapsuleGroupToken(gid);
                const auto it = std::ranges::find(laneEntries, token);
                std::size_t insertAt = it != laneEntries.end() ? static_cast<std::size_t>(it - laneEntries.begin()) + 1
                                                               : laneEntries.size();
                if (emptyNow && it != laneEntries.end()) {
                  const std::size_t pos = static_cast<std::size_t>(it - laneEntries.begin());
                  laneEntries.erase(laneEntries.begin() + static_cast<std::ptrdiff_t>(pos));
                  insertAt = pos;
                  std::vector<BarCapsuleGroupStyle> kept;
                  for (const auto& x : groups) {
                    if (x.id != gid) {
                      kept.push_back(x);
                    }
                  }
                  groups.swap(kept);
                }
                insertAt = std::min(insertAt, laneEntries.size());
                laneEntries.insert(laneEntries.begin() + static_cast<std::ptrdiff_t>(insertAt), ejected);
                setOverrides({{lanePath, laneEntries}, {groupPath, groups}});
              };
            }
            auto memberCard = makeWidgetCard(
                group->members[m], groupZoneIndex, m, inherited, "stack-pop", std::move(eject), false, false,
                std::function<void()>{}
            );
            groupItemNodes->push_back(memberCard.get());
            container->addChild(std::move(memberCard));
          }

          laneItemNodes->push_back(containerPtr);
          lane->addChild(std::move(container));
          continue;
        }

        // Loose widget card.
        const std::string selectionToken = std::string(laneKey) + "#" + std::to_string(i);
        const bool isSelected = std::ranges::contains(ctx.selectedLaneWidgets, selectionToken);
        std::function<void()> removeClose;
        if (!inherited) {
          auto items = laneItems;
          items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
          const bool removeInstance = isGuiManagedNamedWidgetInstance(ctx, entryName)
              && !widgetHasPlacementAfterLaneEdit(ctx.config, lanePath, items, entryName);
          removeClose = [setOverride = ctx.setOverride, clearOverride = ctx.clearOverride, items = std::move(items),
                         lanePath, entryName, removeInstance]() {
            setOverride(lanePath, items);
            if (removeInstance) {
              clearOverride({"widget", entryName});
            }
          };
        }
        std::function<void()> toggleSelect;
        if (!inherited) {
          toggleSelect = [&selectedLaneWidgets = ctx.selectedLaneWidgets, selectionToken,
                          requestRebuild = ctx.requestRebuild]() {
            const auto it = std::ranges::find(selectedLaneWidgets, selectionToken);
            if (it != selectedLaneWidgets.end()) {
              selectedLaneWidgets.erase(it);
            } else {
              selectedLaneWidgets.push_back(selectionToken);
            }
            requestRebuild();
          };
        }
        auto card = makeWidgetCard(
            entryName, laneZoneIndex, i, inherited, "close", std::move(removeClose), !inherited, isSelected,
            std::move(toggleSelect)
        );
        laneItemNodes->push_back(card.get());
        lane->addChild(std::move(card));
      }

      if (laneItems.empty() && !inherited) {
        lane->addChild(
            ui::column(
                {
                    .align = FlexAlign::Center,
                    .gap = 2.0f * ctx.scale,
                    .paddingV = Style::spaceMd * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.25f),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .border = colorSpecFromRole(ColorRole::Outline),
                },
                makeLabel(
                    i18n::tr("settings.entities.widget.lanes.empty"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Bold
                ),
                makeLabel(
                    i18n::tr("settings.entities.widget.lanes.empty-hint"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
                )
            )
        );
      }

      if (!inherited) {
        lane->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.add"),
                .glyph = "add",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusMd(ctx.scale),
                .onClick = [&editingWidgetName = ctx.editingWidgetName, &renamingWidgetName = ctx.renamingWidgetName,
                            &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                            &pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath,
                            openWidgetAddPopup = ctx.openWidgetAddPopup, lanePath]() {
                  editingWidgetName.clear();
                  renamingWidgetName.clear();
                  pendingDeleteWidgetName.clear();
                  pendingDeleteWidgetSettingPath.clear();
                  if (openWidgetAddPopup) {
                    openWidgetAddPopup(lanePath);
                  }
                },
            })
        );
      }

      lanes->addChild(std::move(lane));
    }

    block->addChild(std::move(lanes));
    section.addChild(std::move(block));
  }

} // namespace settings
