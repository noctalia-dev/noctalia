#include "shell/settings/bar_widget_editor.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/scene/node.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace settings {
  namespace {

    constexpr float kDragStartThresholdPx = 6.0f;

    struct LaneWidgetDragState {
      bool active = false;
      bool moved = false;
      float startLocalX = 0.0f;
      float startLocalY = 0.0f;
      float lastLocalX = 0.0f;
      float lastLocalY = 0.0f;
      std::optional<std::size_t> targetLaneIndex;
      std::optional<std::size_t> targetInsertionIndex;
    };

    struct LaneDropTarget {
      std::vector<std::string> path;
      std::vector<std::string> items;
      Flex* lane = nullptr;
      Box* indicator = nullptr;
      std::shared_ptr<std::vector<Flex*>> itemNodes;
    };

    std::unique_ptr<Label> makeLabel(
        std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal
    ) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .color = color,
          .fontWeight = fontWeight,
      });
    }

    std::unique_ptr<Glyph> makeGlyph(std::string_view name, float glyphSize, const ColorSpec& color) {
      return ui::glyph({
          .glyph = std::string(name),
          .glyphSize = glyphSize,
          .color = color,
      });
    }

    std::unique_ptr<Node> makeMiniSectionHeader(std::string_view title, float scale) {
      return ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * scale,
              .configure = [scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0f, 0.0f, 0.0f); },
          },
          ui::separator(),
          makeLabel(title, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
      );
    }

    void closeInspector(
        std::string& editingWidgetName, std::string& renamingWidgetName, std::string& pendingDeleteWidgetName,
        std::string& pendingDeleteWidgetSettingPath, const std::function<void()>& requestRebuild
    ) {
      editingWidgetName.clear();
      renamingWidgetName.clear();
      pendingDeleteWidgetName.clear();
      pendingDeleteWidgetSettingPath.clear();
      requestRebuild();
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

    ColorSpec widgetBadgeColor(WidgetReferenceKind kind) {
      switch (kind) {
      case WidgetReferenceKind::BuiltIn:
        return colorSpecFromRole(ColorRole::Primary, 0.16f);
      case WidgetReferenceKind::Named:
      case WidgetReferenceKind::Preset:
        return colorSpecFromRole(ColorRole::Secondary, 0.18f);
      case WidgetReferenceKind::Unknown:
        return colorSpecFromRole(ColorRole::Error, 0.16f);
      }
      return colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.12f);
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
      items.erase(std::remove(items.begin(), items.end(), key), items.end());
      return items.size() != oldSize;
    }

    void appendReferenceRemoval(
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, std::vector<std::string> path,
        std::vector<std::string> items, std::string_view widgetName
    ) {
      if (removeWidgetReference(items, widgetName)) {
        overrides.push_back({std::move(path), std::move(items)});
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
            overrides.push_back({std::move(path), std::move(items)});
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
      return isNamedWidgetInstance(ctx.config, widgetName) && ctx.configService != nullptr &&
             ctx.configService->hasOverride({"widget", std::string(widgetName)});
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

    std::string widgetCapsuleGroupName(const Config& cfg, std::string_view widgetName) {
      const auto it = cfg.widgets.find(std::string(widgetName));
      if (it == cfg.widgets.end()) {
        return {};
      }

      if (!it->second.hasSetting("capsule_group")) {
        return {};
      }

      return StringUtils::trim(it->second.getString("capsule_group", ""));
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
        std::size_t sourceLaneIndex, std::size_t targetLaneIndex, std::size_t fromIndex, std::size_t insertionIndex
    ) {
      return sourceLaneIndex == targetLaneIndex && (insertionIndex == fromIndex || insertionIndex == fromIndex + 1);
    }

    std::optional<std::size_t>
    laneIndexAtScenePoint(const std::vector<LaneDropTarget>& lanes, float sceneX, float sceneY) {
      for (std::size_t i = 0; i < lanes.size(); ++i) {
        const auto* lane = lanes[i].lane;
        if (lane == nullptr) {
          continue;
        }
        float laneX = 0.0f;
        float laneY = 0.0f;
        Node::absolutePosition(lane, laneX, laneY);
        if (sceneX >= laneX && sceneX < laneX + lane->width() && sceneY >= laneY && sceneY < laneY + lane->height()) {
          return i;
        }
      }
      return std::nullopt;
    }

    void hideDropIndicators(const std::vector<LaneDropTarget>& lanes) {
      for (const auto& lane : lanes) {
        if (lane.indicator != nullptr) {
          lane.indicator->setVisible(false);
        }
      }
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

    std::vector<std::string>
    movedWithinLane(std::vector<std::string> items, std::size_t fromIndex, std::size_t insertionIndex) {
      if (fromIndex >= items.size() || insertionIndex > items.size()) {
        return items;
      }
      if (insertionIndex == fromIndex || insertionIndex == fromIndex + 1) {
        return items;
      }

      auto item = std::move(items[fromIndex]);
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(fromIndex));
      if (insertionIndex > fromIndex) {
        --insertionIndex;
      }
      items.insert(items.begin() + static_cast<std::ptrdiff_t>(insertionIndex), std::move(item));
      return items;
    }

    std::vector<std::string> widgetSettingPath(std::string widgetName, std::string settingKey) {
      return {"widget", std::move(widgetName), std::move(settingKey)};
    }

    WidgetSettingValue
    widgetSettingValue(const Config& cfg, std::string_view widgetName, const WidgetSettingSpec& spec) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(spec.key); settingIt != it->second.settings.end()) {
          return settingIt->second;
        }
      }
      return spec.defaultValue;
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
          if (const auto* b = std::get_if<bool>(&settingIt->second)) {
            return *b ? "true" : "false";
          }
        }
      }
      for (const auto& s : allSpecs) {
        if (s.key == key) {
          if (const auto* str = std::get_if<std::string>(&s.defaultValue)) {
            return *str;
          }
          if (const auto* b = std::get_if<bool>(&s.defaultValue)) {
            return *b ? "true" : "false";
          }
          break;
        }
      }
      return {};
    }

    bool isSettingVisible(
        const Config& cfg, std::string_view widgetName, const WidgetSettingSpec& spec,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (!spec.visibleWhen.has_value()) {
        return true;
      }
      auto matches = [&](const std::string& key, const std::vector<std::string>& values) {
        const auto currentValue = settingCurrentString(cfg, widgetName, key, allSpecs);
        for (const auto& v : values) {
          if (v == currentValue) {
            return true;
          }
        }
        return false;
      };
      for (const auto& condition : spec.visibleWhen->any) {
        if (matches(condition.key, condition.values)) {
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
      return {};
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
            }
          },
          value
      );
    }

    std::vector<SelectOption> managedCapsuleGroupOptions(const Config& cfg, const std::vector<std::string>& lanePath) {
      std::vector<SelectOption> options;
      if (lanePath.size() < 2 || lanePath[0] != "bar") {
        return options;
      }

      const auto* bar = findBar(cfg, lanePath[1]);
      if (bar == nullptr) {
        return options;
      }

      const std::vector<std::string>* groups = &bar->widgetCapsuleGroups;
      if (lanePath.size() >= 4 && lanePath[2] == "monitor") {
        if (const auto* ovr = findMonitorOverride(*bar, lanePath[3]); ovr != nullptr && ovr->widgetCapsuleGroups) {
          groups = &*ovr->widgetCapsuleGroups;
        }
      }

      options.reserve(groups->size() + 1);
      options.push_back(SelectOption{.value = "", .label = i18n::tr("settings.widgets.options.none")});
      for (const auto& group : *groups) {
        if (group.empty()) {
          continue;
        }
        options.push_back(SelectOption{.value = group, .label = group});
      }

      return options;
    }

    [[nodiscard]] bool workspacesMinimalEnabled(
        const Config& cfg, std::string_view widgetName, const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      for (const auto& spec : allSpecs) {
        if (spec.key == "minimal") {
          return settingValueAsBool(widgetSettingValue(cfg, widgetName, spec));
        }
      }
      return false;
    }

    SelectSetting workspacesDisplaySelectSetting(
        const BarWidgetEditorContext& ctx, std::string_view widgetName, const WidgetSettingSpec& displaySpec,
        const std::vector<WidgetSettingSpec>& allSpecs, std::string selectedValue
    ) {
      const bool minimal = workspacesMinimalEnabled(ctx.config, widgetName, allSpecs);
      if (minimal && selectedValue == "none") {
        selectedValue = "id";
      }

      std::vector<SelectOption> options;
      options.reserve(displaySpec.options.size());
      for (const auto& option : displaySpec.options) {
        if (minimal && option.value == "none") {
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

    SelectSetting batteryDeviceSelectSetting(const BarWidgetEditorContext& ctx, std::string selectedValue) {
      if (selectedValue.empty()) {
        selectedValue = "auto";
      }

      std::vector<SelectOption> options = ctx.batteryDeviceOptions;
      if (options.empty()) {
        options.push_back(SelectOption{.value = "auto", .label = i18n::tr("common.states.auto")});
      }

      const auto hasSelected = std::any_of(options.begin(), options.end(), [&selectedValue](const SelectOption& opt) {
        return opt.value == selectedValue;
      });
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
        knownKeys.insert(spec.key);
      }
      // `script` is the identity of a scripted widget, not a raw/deletable extra — when a Lua
      // manifest drives the settings it isn't among the specs, so guard it explicitly.
      if (widgetIt->second.type == "scripted") {
        knownKeys.insert("script");
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
      std::sort(rawKeys.begin(), rawKeys.end());

      panel.addChild(
          ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = 1.0f * ctx.scale,
                  .configure = [&ctx](Flex& flex) { flex.setPadding(Style::spaceXs * ctx.scale, 0.0f); },
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
                .minHeight = Style::controlHeightSm * ctx.scale,
                .configure = [&ctx](Flex& flex) { flex.setPadding(Style::spaceXs * ctx.scale, 0.0f); },
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
        Flex& item, std::string widgetName, const std::vector<std::string>& lanePath,
        const std::vector<SelectOption>& managedCapsuleGroups, const BarWidgetEditorContext& ctx
    ) {
      const auto widgetType = widgetTypeForReference(ctx.config, widgetName);
      if (widgetType.empty()) {
        return;
      }

      const auto widgetIt = ctx.config.widgets.find(widgetName);
      const WidgetConfig* widgetConfig = widgetIt != ctx.config.widgets.end() ? &widgetIt->second : nullptr;
      auto specs = widgetSettingSpecs(widgetType, widgetConfig);
      if (specs.empty()) {
        return;
      }

      auto panel = ui::column(
          {
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * ctx.scale,
              .configure =
                  [&ctx](Flex& flex) {
                    flex.setPadding(Style::spaceSm * ctx.scale);
                    flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                    flex.setFill(colorSpecFromRole(ColorRole::Surface));
                    flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.22f), Style::borderWidth);
                  },
          },
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceXs * ctx.scale,
              },
              makeLabel(
                  i18n::tr("settings.entities.widget.settings.title"), Style::fontSizeCaption * ctx.scale,
                  colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
              ),
              makeLabel(
                  widgetType, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  FontWeight::Normal
              )
          )
      );

      std::size_t visibleSpecs = 0;
      bool groupingHeaderAdded = false;
      for (const auto& spec : specs) {
        if (spec.key == "capsule_group" && managedCapsuleGroups.empty()) {
          continue;
        }
        if (!isSettingVisible(ctx.config, widgetName, spec, specs)) {
          continue;
        }
        if (spec.advanced && !ctx.showAdvanced) {
          continue;
        }
        const auto path = widgetSettingPath(widgetName, spec.key);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        if (ctx.showOverriddenOnly && !overridden) {
          continue;
        }

        if (spec.key == "capsule_group" && !groupingHeaderAdded) {
          panel->addChild(makeMiniSectionHeader(i18n::tr("settings.navigation.groups.grouping"), ctx.scale));
          groupingHeaderAdded = true;
        }

        const auto value = widgetSettingValue(ctx.config, widgetName, spec);
        SettingEntry entry{
            .section = "bar",
            .group = "widget-settings",
            .title = !spec.literalLabel.empty() ? spec.literalLabel
                     : spec.labelKey.empty()    ? std::string{}
                                                : i18n::tr(spec.labelKey),
            .subtitle = !spec.literalDescription.empty() ? spec.literalDescription
                        : spec.descriptionKey.empty()    ? std::string{}
                                                         : i18n::tr(spec.descriptionKey),
            .path = path,
            .control = TextSetting{},
            .advanced = spec.advanced,
            .searchText = {},
            .visibleWhen = std::nullopt,
        };

        switch (spec.valueType) {
        case WidgetSettingValueType::Bool: {
          std::optional<bool> clearWhenValue;
          if (const auto* defaultBool = std::get_if<bool>(&spec.defaultValue)) {
            clearWhenValue = *defaultBool;
          }
          if (widgetType == "workspaces" && spec.key == "minimal") {
            ctx.makeRow(
                *panel, entry,
                ui::toggle({
                    .checked = settingValueAsBool(value),
                    .scale = ctx.scale,
                    .onChange = [configService = ctx.configService, setOverride = ctx.setOverride,
                                 requestRebuild = ctx.requestRebuild, widgetName = std::string(widgetName), path,
                                 displayPath = widgetSettingPath(std::string(widgetName), "display"),
                                 specs](bool enabled) {
                      setOverride(path, enabled);
                      if (enabled && configService != nullptr &&
                          settingCurrentString(configService->config(), widgetName, "display", specs) == "none") {
                        setOverride(displayPath, std::string("id"));
                      }
                      if (requestRebuild) {
                        requestRebuild();
                      }
                    },
                })
            );
          } else {
            ctx.makeRow(*panel, entry, ctx.makeToggle(settingValueAsBool(value), path, clearWhenValue));
          }
          break;
        }
        case WidgetSettingValueType::Int: {
          const auto minValue = static_cast<float>(spec.minValue.value_or(0.0));
          const auto maxValue = static_cast<float>(spec.maxValue.value_or(100.0));
          ctx.makeRow(
              *panel, entry,
              ctx.makeSlider(
                  static_cast<float>(settingValueAsInt(value)), minValue, maxValue, static_cast<float>(spec.step), path,
                  true
              )
          );
          break;
        }
        case WidgetSettingValueType::Double: {
          const auto minValue = static_cast<float>(spec.minValue.value_or(0.0));
          const auto maxValue = static_cast<float>(spec.maxValue.value_or(1.0));
          ctx.makeRow(
              *panel, entry,
              ctx.makeSlider(
                  static_cast<float>(settingValueAsDouble(value)), minValue, maxValue, static_cast<float>(spec.step),
                  path, false
              )
          );
          break;
        }
        case WidgetSettingValueType::OptionalDouble: {
          ctx.makeRow(
              *panel, entry,
              ctx.makeOptionalStepper(
                  OptionalStepperSetting{
                      .value = widgetSettingOptionalStepperValue(ctx.config, widgetName, spec.key),
                      .minValue = static_cast<int>(std::lround(spec.minValue.value_or(0.0))),
                      .maxValue = static_cast<int>(std::lround(spec.maxValue.value_or(80.0))),
                      .step = static_cast<int>(std::max(1.0, spec.step)),
                      .fallbackValue = inheritedCapsuleRadiusForLane(ctx.config, lanePath),
                      .unsetLabel = i18n::tr("common.states.inherit"),
                      .customLabel = i18n::tr("common.states.custom")
                  },
                  path
              )
          );
          break;
        }
        case WidgetSettingValueType::String: {
          auto textNode = ctx.makeText(settingValueAsString(value), {}, path);
          if (spec.key == "glyph") {
            ctx.makeRow(
                *panel, entry,
                ui::row(
                    {
                        .align = FlexAlign::Center,
                        .gap = Style::spaceSm * ctx.scale,
                    },
                    std::move(textNode),
                    ui::button({
                        .glyph = "apps",
                        .glyphSize = Style::fontSizeBody * ctx.scale,
                        .variant = ButtonVariant::Outline,
                        .minWidth = Style::controlHeight * ctx.scale,
                        .minHeight = Style::controlHeight * ctx.scale,
                        .paddingV = Style::spaceXs * ctx.scale,
                        .paddingH = Style::spaceSm * ctx.scale,
                        .radius = Style::scaledRadiusMd(ctx.scale),
                        .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path,
                                    currentValue = settingValueAsString(value)]() {
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
                )
            );
          } else if (spec.key == "capsule_group" && !managedCapsuleGroups.empty()) {
            SelectSetting selectSetting{
                .options = managedCapsuleGroups, .selectedValue = settingValueAsString(value), .clearOnEmpty = true
            };
            ctx.makeRow(*panel, entry, ctx.makeSelect(selectSetting, path));
          } else if (spec.key == "custom_image") {
            ctx.makeRow(
                *panel, entry,
                ui::row(
                    {
                        .align = FlexAlign::Center,
                        .gap = Style::spaceSm * ctx.scale,
                    },
                    std::move(textNode),
                    ui::button({
                        .glyph = "photo",
                        .glyphSize = Style::fontSizeBody * ctx.scale,
                        .variant = ButtonVariant::Outline,
                        .minWidth = Style::controlHeight * ctx.scale,
                        .minHeight = Style::controlHeight * ctx.scale,
                        .paddingV = Style::spaceXs * ctx.scale,
                        .paddingH = Style::spaceSm * ctx.scale,
                        .radius = Style::scaledRadiusMd(ctx.scale),
                        .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path,
                                    currentValue = settingValueAsString(value)]() {
                          FileDialogOptions options;
                          options.mode = FileDialogMode::Open;
                          options.defaultViewMode = FileDialogViewMode::Grid;
                          options.title = i18n::tr("settings.widgets.settings.custom_image.dialog-title");
                          options.extensions = {".png", ".jpg", ".jpeg", ".webp", ".svg", ".bmp", ".gif"};
                          if (!currentValue.empty()) {
                            std::filesystem::path current(currentValue);
                            std::error_code ec;
                            if (current.has_parent_path() && std::filesystem::exists(current.parent_path(), ec)) {
                              options.startDirectory = current.parent_path();
                            }
                          }
                          (void)FileDialog::open(
                              std::move(options),
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
                )
            );
          } else {
            ctx.makeRow(*panel, entry, std::move(textNode));
          }
          break;
        }
        case WidgetSettingValueType::StringList:
          ctx.makeListBlock(*panel, entry, ListSetting{.items = settingValueAsStringList(value)});
          break;
        case WidgetSettingValueType::Select: {
          SelectSetting selectSetting;
          const std::string selectedValue = settingValueAsString(value);
          if (widgetType == "battery" && spec.key == "device") {
            selectSetting = batteryDeviceSelectSetting(ctx, selectedValue);
          } else if (widgetType == "workspaces" && spec.key == "display") {
            selectSetting = workspacesDisplaySelectSetting(ctx, widgetName, spec, specs, selectedValue);
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
          ctx.makeRow(*panel, entry, ctx.makeSelect(std::move(selectSetting), path));
          break;
        }
        case WidgetSettingValueType::ColorSpec: {
          ColorSpecPickerSetting pickerSetting;
          pickerSetting.selectedValue = settingValueAsString(value);
          pickerSetting.allowNone = spec.advanced;
          pickerSetting.allowCustomColor = spec.allowCustomColor;
          ctx.makeRow(*panel, entry, ctx.makeColorSpecPicker(std::move(pickerSetting), path));
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

    void addInspectorPane(Flex& block, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
      static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};

      if (ctx.editingWidgetName.empty()) {
        return;
      }

      auto inspector = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceSm * ctx.scale,
          .configure = [&ctx](Flex& flex) {
            flex.setPadding(Style::spaceMd * ctx.scale);
            flex.setRadius(Style::scaledRadiusMd(ctx.scale));
            flex.setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
            flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.5f), Style::borderWidth);
          },
      });
      if (ctx.setScrollTarget) {
        ctx.setScrollTarget(inspector.get());
      }

      {
        const std::string widgetName = ctx.editingWidgetName;
        const auto info = widgetReferenceInfo(ctx.config, widgetName);
        const std::string capsuleGroup = widgetCapsuleGroupName(ctx.config, widgetName);
        const bool guiManaged = isGuiManagedNamedWidgetInstance(ctx, widgetName);

        std::string currentLaneKey;
        std::vector<std::string> currentLanePath;
        std::vector<std::string> currentLaneItems;
        bool currentLaneInherited = false;
        for (const auto laneKey : kLaneKeys) {
          auto p = pathWithLastSegment(entry.path, std::string(laneKey));
          auto items = barWidgetItemsForPath(ctx.config, p);
          if (std::find(items.begin(), items.end(), widgetName) != items.end()) {
            currentLaneKey = std::string(laneKey);
            currentLanePath = std::move(p);
            currentLaneItems = std::move(items);
            currentLaneInherited = isMonitorWidgetListPath(currentLanePath) &&
                                   !monitorWidgetListHasExplicitValue(ctx.config, currentLanePath);
            break;
          }
        }

        auto headerRow = ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * ctx.scale,
            },
            makeLabel(
                i18n::tr("settings.entities.widget.inspector.edit-title"), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Bold
            )
        );
        {
          auto titleLabel = makeLabel(
              info.title, Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          );
          titleLabel->setMaxLines(1);
          titleLabel->setFlexGrow(1.0f);
          headerRow->addChild(std::move(titleLabel));
        }

        headerRow->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .configure =
                        [&ctx, &info](Flex& flex) {
                          flex.setPadding(0, Style::spaceXs * ctx.scale);
                          flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                          flex.setFill(widgetBadgeColor(info.kind));
                        },
                },
                makeLabel(
                    info.badge, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
                    FontWeight::Bold
                )
            )
        );

        headerRow->addChild(ui::spacer());

        headerRow->addChild(
            ui::button({
                .glyph = "close",
                .glyphSize = Style::fontSizeBody * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minWidth = Style::controlHeightSm * ctx.scale,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .padding = Style::spaceXs * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [&editingWidgetName = ctx.editingWidgetName, &renamingWidgetName = ctx.renamingWidgetName,
                            &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                            &pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath,
                            requestRebuild = ctx.requestRebuild]() {
                  closeInspector(
                      editingWidgetName, renamingWidgetName, pendingDeleteWidgetName, pendingDeleteWidgetSettingPath,
                      requestRebuild
                  );
                },
            })
        );
        inspector->addChild(std::move(headerRow));

        if (!capsuleGroup.empty()) {
          inspector->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceXs * ctx.scale,
                  },
                  makeGlyph(
                      "stack-back", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
                  ),
                  makeLabel(
                      capsuleGroup, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
                      FontWeight::Normal
                  )
              )
          );
        }

        const bool pendingDelete = guiManaged && ctx.pendingDeleteWidgetName == widgetName;
        const bool renaming = guiManaged && ctx.renamingWidgetName == widgetName;

        if (!pendingDelete && !renaming && !currentLaneInherited && !currentLaneKey.empty()) {
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
            auto targetPath = pathWithLastSegment(entry.path, std::string(targetLane));
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
                      auto it = std::find(sourceItems.begin(), sourceItems.end(), widgetName);
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
                    .onClick = [&renamingWidgetName = ctx.renamingWidgetName, widgetName,
                                requestRebuild = ctx.requestRebuild]() {
                      renamingWidgetName = widgetName;
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

          inspector->addChild(std::move(actionRow));
        }

        addWidgetSettingsPanel(
            *inspector, widgetName, currentLanePath, managedCapsuleGroupOptions(ctx.config, currentLanePath), ctx
        );

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
          inspector->addChild(std::move(renameRow));
        }

        if (pendingDelete) {
          auto confirmPanel = ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = Style::spaceXs * ctx.scale,
                  .configure =
                      [&ctx](Flex& flex) {
                        flex.setPadding(Style::spaceSm * ctx.scale);
                        flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                        flex.setFill(colorSpecFromRole(ColorRole::Error, 0.10f));
                        flex.setBorder(colorSpecFromRole(ColorRole::Error, 0.5f), Style::borderWidth);
                      },
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
                      .onClick = [&editingWidgetName = ctx.editingWidgetName,
                                  &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName, config = ctx.config,
                                  widgetName, clearOverride = ctx.clearOverride, setOverrides = ctx.setOverrides]() {
                        pendingDeleteWidgetName.clear();
                        if (editingWidgetName == widgetName) {
                          editingWidgetName.clear();
                        }
                        auto referenceRemovals = widgetReferenceRemovalOverrides(config, widgetName);
                        if (!referenceRemovals.empty()) {
                          setOverrides(std::move(referenceRemovals));
                        }
                        clearOverride({"widget", widgetName});
                      },
                  })
              )
          );
          inspector->addChild(std::move(confirmPanel));
        }
      }

      block.addChild(std::move(inspector));
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

  void addBarWidgetLaneEditor(Flex& section, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
    if (!isFirstBarWidgetListPath(entry.path)) {
      return;
    }

    auto block = ui::column(
        {
            .align = FlexAlign::Stretch,
            .gap = Style::spaceSm * ctx.scale,
            .configure = [&ctx](Flex& flex) { flex.setPadding(2.0f * ctx.scale, 0.0f); },
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

    const bool inspectorActive = !ctx.editingWidgetName.empty();
    if (inspectorActive) {
      addInspectorPane(*block, entry, ctx);
      section.addChild(std::move(block));
      return;
    }

    auto lanes = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * ctx.scale,
        .fillWidth = true,
    });

    static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};
    static constexpr std::size_t kLaneCount = sizeof(kLaneKeys) / sizeof(kLaneKeys[0]);
    auto laneTargets = std::make_shared<std::vector<LaneDropTarget>>();
    laneTargets->reserve(kLaneCount);
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
          .minWidth = 160.0f * ctx.scale,
          .flexGrow = 1.0f,
          .configure = [&ctx](Flex& flex) {
            flex.setPadding(Style::spaceSm * ctx.scale);
            flex.setRadius(Style::scaledRadiusMd(ctx.scale));
            flex.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45f));
            flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.5f), Style::borderWidth);
          },
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

      auto itemNodes = std::make_shared<std::vector<Flex*>>();
      itemNodes->reserve(laneItems.size());
      const std::size_t laneTargetIndex = laneTargets->size();
      laneTargets->push_back(
          LaneDropTarget{
              .path = lanePath,
              .items = laneItems,
              .lane = lanePtr,
              .indicator = dropIndicatorPtr,
              .itemNodes = itemNodes
          }
      );

      auto laneHeader = ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
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
                    .configure =
                        [&ctx](Flex& flex) {
                          flex.setPadding(0, Style::spaceXs * ctx.scale);
                          flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                          flex.setFill(colorSpecFromRole(ColorRole::Primary, 0.15f));
                        },
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
                    .configure =
                        [&ctx](Flex& flex) {
                          flex.setPadding(0, Style::spaceXs * ctx.scale);
                          flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                          flex.setFill(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.14f));
                        },
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
        auto items = laneItems;
        auto path = lanePath;
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

      for (std::size_t i = 0; i < laneItems.size(); ++i) {
        const auto info = widgetReferenceInfo(ctx.config, laneItems[i]);
        const std::string capsuleGroup = widgetCapsuleGroupName(ctx.config, laneItems[i]);
        auto item = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * ctx.scale,
            .configure = [&ctx](Flex& flex) {
              flex.setPadding(Style::spaceXs * ctx.scale, Style::spaceSm * ctx.scale);
              flex.setRadius(Style::scaledRadiusSm(ctx.scale));
              flex.setFill(colorSpecFromRole(ColorRole::Surface, 0.72f));
              flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.22f), Style::borderWidth);
            },
        });
        auto* itemPtr = item.get();
        itemNodes->push_back(itemPtr);

        auto itemTop = ui::row({
            .align = FlexAlign::Center,
            .gap = Style::spaceXs * ctx.scale,
        });
        {
          auto titleLabel = makeLabel(
              info.title, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
          );
          titleLabel->setMaxLines(1);
          titleLabel->setFlexGrow(1.0f);
          itemTop->addChild(std::move(titleLabel));
        }
        itemTop->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .configure =
                        [&ctx, &info](Flex& flex) {
                          flex.setPadding(0, Style::spaceXs * ctx.scale);
                          flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                          flex.setFill(widgetBadgeColor(info.kind));
                        },
                },
                makeLabel(
                    info.badge, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
                    FontWeight::Bold
                )
            )
        );
        if (!capsuleGroup.empty()) {
          itemTop->addChild(makeGlyph(
              "stack-back", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
          ));
        }
        item->addChild(std::move(itemTop));

        if (info.kind != WidgetReferenceKind::BuiltIn && !info.detail.empty()) {
          item->addChild(makeSettingSubtitleLabel(info.detail, ctx.scale));
        }
        if (!capsuleGroup.empty()) {
          item->addChild(
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceXs * ctx.scale,
                  },
                  makeGlyph(
                      "stack-back", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
                  ),
                  makeLabel(
                      capsuleGroup, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
                      FontWeight::Normal
                  )
              )
          );
        }

        auto actions = ui::row({
            .align = FlexAlign::Center,
            .gap = Style::spaceXs * ctx.scale,
        });

        const auto widgetName = laneItems[i];
        const bool editableWidget = !widgetTypeForReference(ctx.config, widgetName).empty();
        if (!inherited) {
          Button* dragBtnPtr = nullptr;
          auto dragBtn = ui::button({
              .out = &dragBtnPtr,
              .glyph = "menu-2",
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Ghost,
              .minWidth = Style::controlHeightSm * ctx.scale,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
          });

          auto dragState = std::make_shared<LaneWidgetDragState>();
          auto items = laneItems;
          auto path = lanePath;
          dragBtn->setOnPress([dragState, itemPtr, laneTargets, setOverride = ctx.setOverride,
                               setOverrides = ctx.setOverrides, items, path, i,
                               laneTargetIndex](float localX, float localY, bool pressed) mutable {
            if (pressed) {
              dragState->active = true;
              dragState->moved = false;
              dragState->startLocalX = localX;
              dragState->startLocalY = localY;
              dragState->lastLocalX = localX;
              dragState->lastLocalY = localY;
              dragState->targetLaneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              itemPtr->setOpacity(0.72f);
              hideDropIndicators(*laneTargets);
              return;
            }

            if (!dragState->active) {
              return;
            }
            dragState->active = false;
            itemPtr->setOpacity(1.0f);
            hideDropIndicators(*laneTargets);
            if (!dragState->moved || !dragState->targetLaneIndex.has_value() ||
                !dragState->targetInsertionIndex.has_value() || laneTargetIndex >= laneTargets->size() ||
                *dragState->targetLaneIndex >= laneTargets->size()) {
              return;
            }

            const std::size_t targetLaneIndex = *dragState->targetLaneIndex;
            std::size_t insertionIndex = *dragState->targetInsertionIndex;
            if (insertionWouldNotMove(laneTargetIndex, targetLaneIndex, i, insertionIndex)) {
              return;
            }

            if (targetLaneIndex == laneTargetIndex) {
              setOverride(path, movedWithinLane(std::move(items), i, insertionIndex));
              return;
            }

            auto sourceItems = items;
            if (i >= sourceItems.size()) {
              return;
            }
            auto movedItem = std::move(sourceItems[i]);
            sourceItems.erase(sourceItems.begin() + static_cast<std::ptrdiff_t>(i));

            const auto& target = (*laneTargets)[targetLaneIndex];
            auto targetItems = target.items;
            insertionIndex = std::min(insertionIndex, targetItems.size());
            targetItems.insert(targetItems.begin() + static_cast<std::ptrdiff_t>(insertionIndex), std::move(movedItem));
            setOverrides({{path, sourceItems}, {target.path, targetItems}});
          });
          dragBtn->setOnPointerMotion([dragState, dragBtnPtr, laneTargets, laneTargetIndex, i,
                                       scale = ctx.scale](float localX, float localY) {
            if (!dragState->active) {
              return;
            }
            dragState->lastLocalX = localX;
            dragState->lastLocalY = localY;
            if (std::hypot(
                    dragState->lastLocalX - dragState->startLocalX, dragState->lastLocalY - dragState->startLocalY
                ) >= kDragStartThresholdPx * scale) {
              dragState->moved = true;
            }
            if (!dragState->moved) {
              return;
            }

            float dragAbsX = 0.0f;
            float dragAbsY = 0.0f;
            Node::absolutePosition(dragBtnPtr, dragAbsX, dragAbsY);
            const float sceneX = dragAbsX + localX;
            const float sceneY = dragAbsY + localY;
            const auto targetLaneIndex = laneIndexAtScenePoint(*laneTargets, sceneX, sceneY);
            if (!targetLaneIndex.has_value() || *targetLaneIndex >= laneTargets->size()) {
              dragState->targetLaneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              hideDropIndicators(*laneTargets);
              return;
            }

            const auto& target = (*laneTargets)[*targetLaneIndex];
            if (target.itemNodes == nullptr || target.lane == nullptr || target.indicator == nullptr) {
              dragState->targetLaneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              hideDropIndicators(*laneTargets);
              return;
            }

            const std::size_t insertionIndex = insertionIndexForSceneY(sceneY, *target.itemNodes);
            if (insertionWouldNotMove(laneTargetIndex, *targetLaneIndex, i, insertionIndex)) {
              dragState->targetLaneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              hideDropIndicators(*laneTargets);
              return;
            }

            dragState->targetLaneIndex = *targetLaneIndex;
            dragState->targetInsertionIndex = insertionIndex;
            hideDropIndicators(*laneTargets);
            updateDropIndicator(*target.indicator, *target.lane, *target.itemNodes, insertionIndex, scale);
          });
          actions->addChild(std::move(dragBtn));
        }
        if (editableWidget) {
          actions->addChild(
              ui::button({
                  .glyph = "settings",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ctx.editingWidgetName == widgetName ? ButtonVariant::Default : ButtonVariant::Ghost,
                  .minWidth = Style::controlHeightSm * ctx.scale,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .padding = Style::spaceXs * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [&editingWidgetName = ctx.editingWidgetName, widgetName,
                              &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                              &renamingWidgetName = ctx.renamingWidgetName,
                              &pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath,
                              requestRebuild = ctx.requestRebuild]() {
                    editingWidgetName = editingWidgetName == widgetName ? std::string{} : widgetName;
                    pendingDeleteWidgetName.clear();
                    pendingDeleteWidgetSettingPath.clear();
                    renamingWidgetName.clear();
                    requestRebuild();
                  },
              })
          );
        }

        if (!inherited) {
          actions->addChild(ui::spacer());

          auto items = laneItems;
          auto path = lanePath;
          actions->addChild(
              ui::button({
                  .glyph = "close",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minWidth = Style::controlHeightSm * ctx.scale,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .padding = Style::spaceXs * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [setOverride = ctx.setOverride, items, path, i]() mutable {
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                    setOverride(path, items);
                  },
              })
          );
        }

        item->addChild(std::move(actions));
        lane->addChild(std::move(item));
      }

      if (laneItems.empty() && !inherited) {
        lane->addChild(
            ui::column(
                {
                    .align = FlexAlign::Center,
                    .gap = 2.0f * ctx.scale,
                    .configure =
                        [&ctx](Flex& flex) {
                          flex.setPadding(Style::spaceMd * ctx.scale, Style::spaceSm * ctx.scale);
                          flex.setRadius(Style::scaledRadiusSm(ctx.scale));
                          flex.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.25f));
                          flex.setBorder(colorSpecFromRole(ColorRole::Outline, 0.18f), Style::borderWidth);
                        },
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
                .radius = Style::scaledRadiusSm(ctx.scale),
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
