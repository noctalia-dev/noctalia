#include "calendar/calendar_service.h"
#include "compositors/compositor_detect.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/ui_phase.h"
#include "dbus/upower/upower_service.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/font_family_catalog.h"
#include "shell/settings/settings_bar_management.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/settings/settings_sidebar.h"
#include "shell/settings/settings_window.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/battery_warning_monitor.h"
#include "system/dependency_service.h"
#include "theme/builtin_templates.h"
#include "theme/community_palettes.h"
#include "theme/community_templates.h"
#include "theme/custom_palettes.h"
#include "ui/builders.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

  constexpr Logger kLog("settings");

  constexpr auto kSearchDebounceInterval = std::chrono::milliseconds(120);

  bool useLightPalettePreview(ThemeMode mode) { return mode == ThemeMode::Light; }

  ColorSwatchPreview palettePreviewFromMetadata(const noctalia::theme::AvailablePalette::PreviewMode& metadata) {
    ColorSwatchPreview preview;
    Color surface;
    if (tryParseHexColor(metadata.surface, surface)) {
      preview.surface = fixedColorSpec(surface);
    }
    preview.swatches.reserve(metadata.accents.size());
    for (const auto& hexColor : metadata.accents) {
      Color color;
      if (tryParseHexColor(hexColor, color)) {
        preview.swatches.push_back(fixedColorSpec(color));
      }
    }
    return preview;
  }

  ColorSwatchPreview availablePalettePreview(const noctalia::theme::AvailablePalette& palette, ThemeMode mode) {
    if (useLightPalettePreview(mode)) {
      ColorSwatchPreview preview = palettePreviewFromMetadata(palette.preview.light);
      if (!preview.empty()) {
        return preview;
      }
      return palettePreviewFromMetadata(palette.preview.dark);
    }
    return palettePreviewFromMetadata(palette.preview.dark);
  }

  std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  std::unique_ptr<Flex> centeredRow(std::unique_ptr<Flex> child) {
    child->setFlexGrow(1.0f);
    return ui::row(
        {
            .align = FlexAlign::Stretch,
            .justify = FlexJustify::Center,
        },
        std::move(child)
    );
  }

  std::vector<settings::SettingsSection> sectionKeys(const std::vector<settings::SettingEntry>& entries) {
    std::vector<settings::SettingsSection> sections;
    for (const auto& descriptor : settings::settingsSectionDescriptors()) {
      if (!descriptor.sidebar) {
        continue;
      }
      const bool present = descriptor.alwaysShow
          || std::ranges::find_if(
                 entries, [section = descriptor.section](const settings::SettingEntry& entry) {
                   return entry.section == section;
                 }
             ) != entries.end();
      if (present) {
        sections.push_back(descriptor.section);
      }
    }
    return sections;
  }

  bool containsPath(const std::vector<std::vector<std::string>>& paths, const std::vector<std::string>& path) {
    return std::ranges::contains(paths, path);
  }

  bool settingPathMayReshapeRegistry(const std::vector<std::string>& path) {
    if (path.empty()) {
      return true;
    }
    if (path[0] == "bar" || path[0] == "widget" || path[0] == "plugins" || path[0] == "plugin_settings") {
      return true;
    }
    if (path.size() >= 2 && path[0] == "theme" && (path[1] == "mode" || path[1] == "source")) {
      return true;
    }
    if (path.size() >= 2 && path[0] == "calendar" && path[1] == "account") {
      return true;
    }
    if (path.size() >= 2 && path[0] == "shell" && path[1] == "greeter_sync") {
      return true;
    }
    return false;
  }

  std::optional<double> overrideNumber(const ConfigOverrideValue& value) {
    if (const auto* v = std::get_if<double>(&value)) {
      return *v;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      return static_cast<double>(*v);
    }
    return std::nullopt;
  }

  std::optional<int> overrideInt(const ConfigOverrideValue& value) {
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      return static_cast<int>(*v);
    }
    if (const auto* v = std::get_if<double>(&value)) {
      return static_cast<int>(std::lround(*v));
    }
    return std::nullopt;
  }

  std::optional<std::string> overrideSelectValue(const ConfigOverrideValue& value) {
    if (const auto* v = std::get_if<std::string>(&value)) {
      return *v;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
      return std::to_string(*v);
    }
    if (const auto* v = std::get_if<bool>(&value)) {
      return *v ? "true" : "false";
    }
    return std::nullopt;
  }

  bool patchSettingEntryValue(
      settings::SettingEntry& entry, const std::vector<std::string>& path, const ConfigOverrideValue& value
  ) {
    if (auto* range = std::get_if<settings::RangeSliderSetting>(&entry.control)) {
      const auto number = overrideNumber(value);
      if (!number.has_value()) {
        return false;
      }
      if (entry.path == path) {
        range->lowValue = *number;
        return true;
      }
      if (range->highPath == path) {
        range->highValue = *number;
        return true;
      }
      return false;
    }

    if (entry.path != path) {
      return false;
    }

    if (auto* toggle = std::get_if<settings::ToggleSetting>(&entry.control)) {
      const auto* checked = std::get_if<bool>(&value);
      if (checked == nullptr) {
        return false;
      }
      toggle->checked = *checked;
      return true;
    }
    if (auto* select = std::get_if<settings::SelectSetting>(&entry.control)) {
      auto selected = overrideSelectValue(value);
      if (!selected.has_value()) {
        return false;
      }
      select->selectedValue = std::move(*selected);
      return true;
    }
    if (auto* picker = std::get_if<settings::SearchPickerSetting>(&entry.control)) {
      const auto* selected = std::get_if<std::string>(&value);
      if (selected == nullptr) {
        return false;
      }
      picker->selectedValue = *selected;
      return true;
    }
    if (auto* slider = std::get_if<settings::SliderSetting>(&entry.control)) {
      const auto number = overrideNumber(value);
      if (!number.has_value()) {
        return false;
      }
      slider->value = *number;
      return true;
    }
    if (auto* text = std::get_if<settings::TextSetting>(&entry.control)) {
      const auto* next = std::get_if<std::string>(&value);
      if (next == nullptr) {
        return false;
      }
      text->value = *next;
      return true;
    }
    if (auto* optionalNumber = std::get_if<settings::OptionalNumberSetting>(&entry.control)) {
      if (auto number = overrideNumber(value); number.has_value()) {
        optionalNumber->value = number;
        return true;
      }
      const auto* text = std::get_if<std::string>(&value);
      if (text != nullptr && (*text == "auto" || text->empty())) {
        optionalNumber->value = std::nullopt;
        return true;
      }
      return false;
    }
    if (auto* optionalStepper = std::get_if<settings::OptionalStepperSetting>(&entry.control)) {
      if (auto number = overrideInt(value); number.has_value()) {
        optionalStepper->value = number;
        return true;
      }
      const auto* text = std::get_if<std::string>(&value);
      if (text != nullptr && (*text == "auto" || text->empty())) {
        optionalStepper->value = std::nullopt;
        return true;
      }
      return false;
    }
    if (auto* stepper = std::get_if<settings::StepperSetting>(&entry.control)) {
      const auto number = overrideInt(value);
      if (!number.has_value()) {
        return false;
      }
      stepper->value = *number;
      return true;
    }
    if (auto* list = std::get_if<settings::ListSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<std::string>>(&value);
      if (items == nullptr) {
        return false;
      }
      list->items = *items;
      return true;
    }
    if (auto* shortcuts = std::get_if<settings::ShortcutListSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<ShortcutConfig>>(&value);
      if (items == nullptr) {
        return false;
      }
      shortcuts->items = *items;
      return true;
    }
    if (auto* keybinds = std::get_if<settings::KeybindListSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<KeyChord>>(&value);
      if (items == nullptr) {
        return false;
      }
      keybinds->items = *items;
      return true;
    }
    if (auto* sessionActions = std::get_if<settings::SessionPanelActionsSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<SessionPanelActionConfig>>(&value);
      if (items == nullptr) {
        return false;
      }
      sessionActions->items = *items;
      return true;
    }
    if (auto* idle = std::get_if<settings::IdleBehaviorsSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<IdleBehaviorConfig>>(&value);
      if (items == nullptr) {
        return false;
      }
      idle->items = *items;
      return true;
    }
    if (auto* filters = std::get_if<settings::NotificationFiltersSetting>(&entry.control)) {
      const auto* items = std::get_if<std::vector<NotificationFilterConfig>>(&value);
      if (items == nullptr) {
        return false;
      }
      filters->items = *items;
      return true;
    }
    if (auto* multi = std::get_if<settings::MultiSelectSetting>(&entry.control)) {
      const auto* stored = std::get_if<std::vector<std::string>>(&value);
      if (stored == nullptr) {
        return false;
      }
      if (multi->persistUnselected) {
        // The override stores the unchecked complement (denylist); reconstruct the
        // selection as every option not present in it.
        std::vector<std::string> selected;
        selected.reserve(multi->options.size());
        for (const auto& option : multi->options) {
          if (!std::ranges::contains(*stored, option.value)) {
            selected.push_back(option.value);
          }
        }
        multi->selectedValues = std::move(selected);
      } else {
        multi->selectedValues = *stored;
      }
      return true;
    }
    if (auto* grid = std::get_if<settings::TemplateGridSetting>(&entry.control)) {
      const auto* selected = std::get_if<std::vector<std::string>>(&value);
      if (selected == nullptr) {
        return false;
      }
      grid->selectedValues = *selected;
      return true;
    }
    if (auto* color = std::get_if<settings::ColorSpecPickerSetting>(&entry.control)) {
      const auto* selected = std::get_if<std::string>(&value);
      if (selected == nullptr) {
        return false;
      }
      color->selectedValue = *selected;
      return true;
    }
    return false;
  }

  bool patchSettingsRegistryValue(
      std::vector<settings::SettingEntry>& registry, const std::vector<std::string>& path,
      const ConfigOverrideValue& value
  ) {
    if (settingPathMayReshapeRegistry(path)) {
      return false;
    }

    bool patched = false;
    for (auto& entry : registry) {
      const bool pathMatches = entry.path == path
          || (std::holds_alternative<settings::RangeSliderSetting>(entry.control)
              && std::get<settings::RangeSliderSetting>(entry.control).highPath == path);
      if (!pathMatches) {
        continue;
      }
      if (!patchSettingEntryValue(entry, path, value)) {
        return false;
      }
      patched = true;
    }
    return patched;
  }

  std::optional<toml::table> configSectionTable(const Config& cfg, std::string_view section) {
    namespace schema = noctalia::config::schema;

    if (section == "audio") {
      return schema::writeTable(cfg.audio, schema::audioSchema());
    }
    if (section == "backdrop") {
      return schema::writeTable(cfg.backdrop, schema::backdropSchema());
    }
    if (section == "battery") {
      return schema::writeTable(cfg.battery, schema::batterySchema());
    }
    if (section == "brightness") {
      return schema::writeTable(cfg.brightness, schema::brightnessSchema());
    }
    if (section == "calendar") {
      return schema::writeTable(cfg.calendar, schema::calendarSchema());
    }
    if (section == "control_center") {
      return schema::writeTable(cfg.controlCenter, schema::controlCenterSchema());
    }
    if (section == "dock") {
      return schema::writeTable(cfg.dock, schema::dockSchema());
    }
    if (section == "desktop_widgets") {
      return schema::writeTable(cfg.desktopWidgets, schema::desktopWidgetsSchema());
    }
    if (section == "hooks") {
      return schema::writeTable(cfg.hooks, schema::hooksSchema());
    }
    if (section == "hot_corners") {
      return schema::writeTable(cfg.hotCorners, schema::hotCornersSchema());
    }
    if (section == "idle") {
      return schema::writeTable(cfg.idle, schema::idleSchema());
    }
    if (section == "keybinds") {
      return schema::writeTable(cfg.keybinds, schema::keybindsSchema());
    }
    if (section == "location") {
      return schema::writeTable(cfg.location, schema::locationSchema());
    }
    if (section == "lockscreen") {
      return schema::writeTable(cfg.lockscreen, schema::lockscreenSchema());
    }
    if (section == "lockscreen_widgets") {
      return schema::writeTable(cfg.lockscreenWidgets, schema::lockscreenWidgetsSchema());
    }
    if (section == "nightlight") {
      return schema::writeTable(cfg.nightlight, schema::nightlightSchema());
    }
    if (section == "notification") {
      return schema::writeTable(cfg.notification, schema::notificationSchema());
    }
    if (section == "osd") {
      return schema::writeTable(cfg.osd, schema::osdSchema());
    }
    if (section == "plugins") {
      return schema::writeTable(cfg.plugins, schema::pluginsSchema());
    }
    if (section == "shell") {
      return schema::writeTable(cfg.shell, schema::shellSchema());
    }
    if (section == "system") {
      return schema::writeTable(cfg.system, schema::systemSchema());
    }
    if (section == "theme") {
      return schema::writeTable(cfg.theme, schema::themeSchema());
    }
    if (section == "wallpaper") {
      return schema::writeTable(cfg.wallpaper, schema::wallpaperSchema());
    }
    if (section == "weather") {
      return schema::writeTable(cfg.weather, schema::weatherSchema());
    }
    return std::nullopt;
  }

  const toml::node* findSectionValue(const toml::table& sectionTable, const std::vector<std::string>& path) {
    if (path.size() < 2) {
      return nullptr;
    }

    const toml::node* node = &sectionTable;
    for (std::size_t i = 1; i < path.size(); ++i) {
      const auto* table = node->as_table();
      if (table == nullptr) {
        return nullptr;
      }
      node = table->get(path[i]);
      if (node == nullptr) {
        return nullptr;
      }
    }
    return node;
  }

  std::optional<ConfigOverrideValue> configOverrideValueFromNode(const toml::node& node) {
    if (auto value = node.value<bool>()) {
      return ConfigOverrideValue{*value};
    }
    if (auto value = node.value<std::int64_t>()) {
      return ConfigOverrideValue{*value};
    }
    if (auto value = node.value<double>()) {
      return ConfigOverrideValue{*value};
    }
    if (auto value = node.value<std::string>()) {
      return ConfigOverrideValue{*value};
    }

    const auto* array = node.as_array();
    if (array == nullptr) {
      return std::nullopt;
    }

    std::vector<std::string> values;
    values.reserve(array->size());
    for (const auto& item : *array) {
      auto value = item.value<std::string>();
      if (!value.has_value()) {
        return std::nullopt;
      }
      values.push_back(std::move(*value));
    }
    return ConfigOverrideValue{std::move(values)};
  }

  bool patchSettingsRegistryResetValues(
      std::vector<settings::SettingEntry>& registry, const Config& cfg,
      const std::vector<std::vector<std::string>>& paths
  ) {
    if (paths.empty()) {
      return false;
    }

    std::optional<std::string_view> currentSection;
    std::optional<toml::table> sectionTable;
    for (const auto& path : paths) {
      if (settingPathMayReshapeRegistry(path) || path.empty()) {
        return false;
      }
      if (!currentSection.has_value() || *currentSection != path[0]) {
        sectionTable = configSectionTable(cfg, path[0]);
        if (!sectionTable.has_value()) {
          return false;
        }
        currentSection = path[0];
      }

      const toml::node* node = findSectionValue(*sectionTable, path);
      if (node == nullptr) {
        return false;
      }
      auto value = configOverrideValueFromNode(*node);
      if (!value.has_value() || !patchSettingsRegistryValue(registry, path, *value)) {
        return false;
      }
    }
    return true;
  }

  class SettingsProfileWatch {
  public:
    SettingsProfileWatch() {
      if (noctalia::profiling::enabled()) {
        m_watch.emplace();
      }
    }

    void reset() {
      if (m_watch.has_value()) {
        m_watch->reset();
      }
    }

    [[nodiscard]] bool active() const noexcept { return m_watch.has_value(); }
    [[nodiscard]] double elapsedMs() const { return m_watch.has_value() ? m_watch->elapsedMs() : 0.0; }

  private:
    std::optional<noctalia::profiling::StopWatch> m_watch;
  };

  void logSettingsProfile(std::string_view label, const SettingsProfileWatch& watch) {
    if (watch.active()) {
      kLog.info("profile {}: {:.1f}ms", label, watch.elapsedMs());
    }
  }

  bool settingEntryBelongsToPage(
      const settings::SettingEntry& entry, std::string_view selectedSection, std::string_view selectedBarName,
      std::string_view selectedMonitorOverride
  ) {
    if (selectedSection != "bar") {
      const auto section = settings::settingsSectionFromId(selectedSection);
      return section.has_value() && entry.section == *section;
    }

    if (entry.section != settings::SettingsSection::Bar
        || entry.path.size() < 2
        || entry.path[0] != "bar"
        || entry.path[1] != selectedBarName) {
      return false;
    }

    const bool entryIsMonitorOverride = entry.path.size() >= 5 && entry.path[2] == "monitor";
    if (selectedMonitorOverride.empty()) {
      return !entryIsMonitorOverride;
    }
    return entryIsMonitorOverride && entry.path[3] == selectedMonitorOverride;
  }

  std::string pageScopeKey(
      std::string_view selectedSection, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (selectedSection != "bar") {
      return std::string(selectedSection);
    }
    std::string key = "bar:" + std::string(selectedBarName);
    if (!selectedMonitorOverride.empty()) {
      key += ":monitor:" + std::string(selectedMonitorOverride);
    }
    return key;
  }

  std::string upowerDeviceLabel(const UPowerDeviceInfo& device) {
    const std::string nativeName =
        !device.nativePath.empty() ? StringUtils::pathTail(device.nativePath) : StringUtils::pathTail(device.path);

    std::string label;
    if (!device.vendor.empty() && !device.model.empty()) {
      label = device.vendor + " " + device.model;
    } else if (!device.model.empty()) {
      label = device.model;
    } else if (!device.vendor.empty()) {
      label = device.vendor;
    } else {
      label = nativeName;
    }

    if (!nativeName.empty() && label != nativeName) {
      label += " (" + nativeName + ")";
    }
    return label;
  }

  std::vector<settings::SelectOption> upowerBatteryDeviceOptions(UPowerService* upower) {
    std::vector<settings::SelectOption> options;
    options.push_back(settings::SelectOption{.value = "auto", .label = i18n::tr("common.states.auto")});
    if (upower == nullptr) {
      return options;
    }

    const auto devices = upower->batteryDevices();
    options.reserve(devices.size() + 1);
    for (const auto& device : devices) {
      std::string description = device.path;
      if (!device.nativePath.empty() && device.nativePath != device.path) {
        description = device.nativePath + " - " + device.path;
      }
      options.push_back(
          settings::SelectOption{
              .value = device.path,
              .label = upowerDeviceLabel(device),
              .description = std::move(description),
          }
      );
    }
    return options;
  }

  std::vector<settings::SelectOption> discoverFontFamilyOptions() {
    const std::vector<std::string>& families = settings::discoverFontFamilies();
    std::vector<settings::SelectOption> options;
    options.reserve(families.size());
    for (const std::string& family : families) {
      options.push_back(settings::SelectOption{family, family});
    }
    return options;
  }

} // namespace

void SettingsWindow::applyPendingContentScrollTarget(float margin) {
  if (!m_scrollToPendingContentTarget) {
    return;
  }

  auto clearPending = [this]() {
    m_scrollToPendingContentTarget = false;
    m_pendingContentScrollTarget = nullptr;
  };

  if (m_contentScrollView == nullptr
      || m_contentScrollView->content() == nullptr
      || m_pendingContentScrollTarget == nullptr) {
    clearPending();
    return;
  }

  scrollNodeIntoScrollView(*m_contentScrollView, &m_contentScrollState, *m_pendingContentScrollTarget, margin);
  clearPending();
}

void SettingsWindow::scrollSidebarNodeIntoView(const Node* node) {
  if (node == nullptr || m_sidebarScrollView == nullptr) {
    return;
  }
  scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *node, Style::spaceXs * uiScale());
}

void SettingsWindow::scrollFocusedAreaIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  if (m_contentScrollView != nullptr && m_contentScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_contentScrollView->content()) {
        m_pendingContentScrollTarget = area;
        m_scrollToPendingContentTarget = true;
        if (!m_deferFocusScrollToLayout) {
          applyPendingContentScrollTarget(Style::spaceMd * uiScale());
        }
        return;
      }
    }
  }

  if (m_sidebarScrollView != nullptr && m_sidebarScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_sidebarScrollView->content()) {
        scrollSidebarNodeIntoView(area);
        return;
      }
    }
  }
}

settings::RegistryEnvironment SettingsWindow::buildRegistryEnvironment() const {
  settings::RegistryEnvironment env;
  if (m_config != nullptr) {
    env.shellAvatarPath = shell::resolvedAvatarPath(m_accounts, m_config->config());
  }
  env.niriBackdropSupported = (m_wayland != nullptr && compositors::isNiri());
  env.screencopySupported = m_wayland != nullptr && m_wayland->hasScreencopy();
  env.niriOverviewTypeToLaunchSupported = (m_wayland != nullptr && compositors::isNiri());
  env.ddcutilAvailable = (m_dependencies != nullptr && m_dependencies->hasDdcutil());
  env.systemdUserManaged = process::runningUnderSystemdUserManager();
  env.gammaControlAvailable = (m_wayland != nullptr && m_wayland->hasGammaControl());
  env.greeterSyncAvailable =
      m_config != nullptr && greeter::appearanceSyncAvailable(m_config->config().shell.greeterSync);
  const ThemeMode previewMode = m_config != nullptr ? m_config->config().theme.mode : ThemeMode::Dark;
  for (const auto& paletteInfo : noctalia::theme::availableCommunityPalettes()) {
    env.communityPalettes.push_back(
        settings::SelectOption{
            .value = paletteInfo.name,
            .label = paletteInfo.name,
            .description = {},
            .preview = availablePalettePreview(paletteInfo, previewMode),
        }
    );
  }
  for (const auto& p : noctalia::theme::availableCustomPalettes()) {
    env.customPalettes.push_back(
        settings::SelectOption{
            .value = p.name,
            .label = p.name,
            .description = {},
            .preview = availablePalettePreview(p, previewMode),
        }
    );
  }
  for (const auto& t : noctalia::theme::CommunityTemplateService::availableTemplates()) {
    env.communityTemplates.push_back(
        settings::SelectOption{
            .value = t.id,
            .label = t.displayName,
            .description = t.category,
            .tooltip = noctalia::theme::formatTemplateTooltip(t)
        }
    );
  }
  static const std::vector<settings::SelectOption> kFontFamilies = discoverFontFamilyOptions();
  env.fontFamilies = kFontFamilies;
  const auto allBatteryDeviceOptions = upowerBatteryDeviceOptions(m_upower);
  const auto* systemBattery = m_upower != nullptr ? m_upower->defaultSystemBattery() : nullptr;
  env.systemBatteryAvailable = systemBattery != nullptr;
  for (const auto& option : allBatteryDeviceOptions) {
    if (option.value == "auto" || (systemBattery != nullptr && option.value == systemBattery->path)) {
      continue;
    }
    env.batteryDeviceOptions.push_back(option);
  }
  env.batteryAvailable = env.systemBatteryAvailable || !env.batteryDeviceOptions.empty();
  if (env.batteryAvailable && m_config != nullptr) {
    for (const auto& option : env.batteryDeviceOptions) {
      env.batteryWarningThresholds.emplace(
          option.value, batteryWarningThresholdForSelector(m_config->config().battery, m_upower, option.value)
      );
    }
  }
  if (m_wayland != nullptr) {
    for (const auto& output : m_wayland->outputs()) {
      if (output.output == nullptr || output.connectorName.empty()) {
        continue;
      }
      std::string label = output.connectorName;
      if (!output.description.empty()) {
        label += " (" + output.description + ")";
      }
      env.availableOutputs.push_back(settings::SelectOption{output.connectorName, std::move(label)});
    }
  }
  return env;
}

void SettingsWindow::syncSelectedBarState(const Config& cfg, const std::vector<std::string>& availableBars) {
  if (availableBars.empty()) {
    m_selectedBarName.clear();
  } else if (settings::findBar(cfg, m_selectedBarName) == nullptr) {
    m_selectedBarName = availableBars.front();
  }

  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  if (selectedBar != nullptr
      && !m_selectedMonitorOverride.empty()
      && settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride) == nullptr) {
    m_selectedMonitorOverride.clear();
  }
}

std::vector<settings::SelectOption> SettingsWindow::batteryDeviceOptions() const {
  return upowerBatteryDeviceOptions(m_upower);
}

settings::SettingsContentContext SettingsWindow::makeContentContext(
    const Config& cfg, const BarConfig* selectedBar, const BarMonitorOverride* selectedMonitorOverride
) {
  const auto requestRebuild = [this]() { requestSceneRebuild(); };
  const auto requestContent = [this]() { requestContentRebuild(); };
  const auto setOverride = [this](std::vector<std::string> path, ConfigOverrideValue value) {
    setSettingOverride(std::move(path), std::move(value));
  };
  const auto setOverrides = [this](std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides) {
    setSettingOverrides(std::move(overrides));
  };
  const auto clearOverride = [this](std::vector<std::string> path) { clearSettingOverride(std::move(path)); };
  const auto clearOverrides = [this](std::vector<std::vector<std::string>> paths) {
    clearSettingOverrides(std::move(paths));
  };
  const auto renameWidget = [this](
                                std::string oldName, std::string newName,
                                std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
                            ) {
    renameWidgetInstance(std::move(oldName), std::move(newName), std::move(referenceOverrides));
  };

  return settings::SettingsContentContext{
      .config = cfg,
      .configService = m_config,
      .scale = uiScale(),
      .searchQuery = m_searchQuery,
      .selectedSection = m_selectedSection,
      .selectedBar = selectedBar,
      .selectedMonitorOverride = selectedMonitorOverride,
      .showAdvanced = m_showAdvanced,
      .showOverriddenOnly = m_showOverriddenOnly,
      .batteryDeviceOptions = batteryDeviceOptions(),
      .keyboardLayoutNames = m_wayland != nullptr ? m_wayland->keyboardLayoutNames() : std::vector<std::string>{},
      .editingWidgetName = m_editingWidgetName,
      .editingCapsuleGroupId = m_editingCapsuleGroupId,
      .selectedLaneWidgets = m_selectedLaneWidgets,
      .pendingDeleteWidgetName = m_pendingDeleteWidgetName,
      .pendingDeleteWidgetSettingPath = m_pendingDeleteWidgetSettingPath,
      .renamingWidgetName = m_renamingWidgetName,
      .requestRebuild = requestRebuild,
      .requestContentRebuild = requestContent,
      .resetContentScroll = [this]() { m_contentScrollState.offset = 0.0f; },
      .setScrollTarget = [this](Node* target) { m_pendingContentScrollTarget = target; },
      .focusArea = [this](InputArea* area) { m_inputDispatcher.setFocus(area); },
      .openBarWidgetAddPopup = [this](const std::vector<std::string>& lanePath) { openBarWidgetAddPopup(lanePath); },
      .openSearchPickerPopup =
          [this](settings::SearchPickerOpenRequest request) { openSearchPickerPopup(std::move(request)); },
      .setOverride = setOverride,
      .setOverrides = setOverrides,
      .clearOverride = clearOverride,
      .clearOverrides = clearOverrides,
      .isResetConfirmationPending =
          [this](const std::vector<std::vector<std::string>>& paths) { return m_pendingResetSettingPaths == paths; },
      .requestResetConfirmation =
          [this](std::vector<std::vector<std::string>> paths) {
            m_pendingResetPageScope.clear();
            m_pendingResetSettingPaths = std::move(paths);
          },
      .renameWidgetInstance = renameWidget,
      .openSessionActionEntryEditor = [this](std::size_t entryIndex) { openSessionActionEntryEditor(entryIndex); },
      .openIdleBehaviorEntryEditor = [this](std::size_t entryIndex) { openIdleBehaviorEntryEditor(entryIndex); },
      .openIdleBehaviorCreateEditor = [this]() { openIdleBehaviorCreateEditor(); },
      .openNotificationFilterEntryEditor =
          [this](std::size_t entryIndex) { openNotificationFilterEntryEditor(entryIndex); },
      .openNotificationFilterCreateEditor = [this]() { openNotificationFilterCreateEditor(); },
      .openWidgetInspectorEditor = [this](
                                       std::vector<std::string> laneListPath, std::string widgetName
                                   ) { openWidgetInspectorEditor(std::move(laneListPath), std::move(widgetName)); },
      .openCapsuleGroupEditor = [this](
                                    std::vector<std::string> laneListPath, std::string groupId
                                ) { openCapsuleGroupEditor(std::move(laneListPath), std::move(groupId)); },
      .registerIdleLiveStatusLabel =
          [this](Label* label) {
            m_idleLiveStatusLabel = label;
            refreshIdleLiveStatusText();
          },
      .registerSessionActionSummaryLabel =
          [this](std::size_t index, Label* label) {
            if (index >= m_sessionActionSummaryLabels.size()) {
              m_sessionActionSummaryLabels.resize(index + 1, nullptr);
            }
            m_sessionActionSummaryLabels[index] = label;
          },
      .bindSessionActionsEditState = [this](
                                         std::shared_ptr<std::vector<SessionPanelActionConfig>> state
                                     ) { m_sessionActionsEditState = std::move(state); },
      .afterSessionActionsCommit = {},
      .afterIdleBehaviorApply = {},
      .afterNotificationFilterApply = {},
      .closeHostedEditor = {},
      .supportsTaskbarWorkspaceGrouping = m_platform != nullptr && m_platform->supportsTaskbarWorkspaceGrouping(),
  };
}

void SettingsWindow::syncSessionActionInlineSummary(std::size_t index, const SessionPanelActionConfig& row) {
  if (index >= m_sessionActionSummaryLabels.size()) {
    return;
  }
  Label* label = m_sessionActionSummaryLabels[index];
  if (label == nullptr) {
    return;
  }
  label->setText(settings::sessionActionDisplayTitle(row));
}

void SettingsWindow::rebuildSettingsContent() {
  uiAssertNotRendering("SettingsWindow::rebuildSettingsContent");
  if (m_contentContainer == nullptr) {
    return;
  }
  SettingsProfileWatch totalProfileWatch;
  SettingsProfileWatch phaseProfileWatch;

  m_pendingContentScrollTarget = nullptr;
  m_idleLiveStatusLabel = nullptr;
  m_sessionActionSummaryLabels.clear();
  m_sessionActionsEditState.reset();
  while (!m_contentContainer->children().empty()) {
    m_contentContainer->removeChild(m_contentContainer->children().back().get());
  }
  logSettingsProfile("rebuildContent clear", phaseProfileWatch);
  phaseProfileWatch.reset();

  const float scale = uiScale();
  const Config fallbackCfg{};
  const Config& cfg = m_config != nullptr ? m_config->config() : fallbackCfg;
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  m_contentContainer->setDirection(FlexDirection::Vertical);
  m_contentContainer->setAlign(FlexAlign::Stretch);
  m_contentContainer->setGap(Style::spaceMd * scale);
  logSettingsProfile("rebuildContent setup", phaseProfileWatch);
  phaseProfileWatch.reset();

  settings::addSettingsBarManagement(
      *m_contentContainer,
      settings::SettingsBarManagementContext{
          .config = cfg,
          .configService = m_config,
          .scale = scale,
          .searchQuery = m_searchQuery,
          .selectedSection = m_selectedSection,
          .selectedBar = selectedBar,
          .selectedMonitorOverride = selectedMonitorOverride,
          .renamingBarName = m_renamingBarName,
          .pendingDeleteBarName = m_pendingDeleteBarName,
          .renamingMonitorOverrideBarName = m_renamingMonitorOverrideBarName,
          .renamingMonitorOverrideMatch = m_renamingMonitorOverrideMatch,
          .pendingDeleteMonitorOverrideBarName = m_pendingDeleteMonitorOverrideBarName,
          .pendingDeleteMonitorOverrideMatch = m_pendingDeleteMonitorOverrideMatch,
          .requestRebuild = [this]() { requestSceneRebuild(); },
          .renameBar =
              [this](std::string oldName, std::string newName) { renameBar(std::move(oldName), std::move(newName)); },
          .deleteBar = [this](std::string name) { deleteBar(std::move(name)); },
          .moveBar = [this](std::string name, int direction) { moveBar(std::move(name), direction); },
          .renameMonitorOverride =
              [this](std::string barName, std::string oldMatch, std::string newMatch) {
                renameMonitorOverride(std::move(barName), std::move(oldMatch), std::move(newMatch));
              },
          .deleteMonitorOverride = [this](
                                       std::string barName, std::string match
                                   ) { deleteMonitorOverride(std::move(barName), std::move(match)); },
      }
  );
  logSettingsProfile("rebuildContent barManagement", phaseProfileWatch);
  phaseProfileWatch.reset();

  const std::size_t visibleEntries = settings::addSettingsContentSections(
      *m_contentContainer, m_settingsRegistry, makeContentContext(cfg, selectedBar, selectedMonitorOverride)
  );
  logSettingsProfile("rebuildContent sections", phaseProfileWatch);
  phaseProfileWatch.reset();

  if (m_selectedSection == "plugins" && m_pluginManager != nullptr) {
    refreshPluginListIfNeeded();
    settings::addSettingsPlugins(
        *m_contentContainer,
        settings::SettingsPluginsContext{
            .scale = scale,
            .selectedSection = m_selectedSection,
            .plugins = m_pluginList,
            .sources = cfg.plugins.sources,
            .pluginsLoading = m_pluginListDirty || m_pluginListRefreshInFlight,
            .setEnabled =
                [this](std::string id, bool enable) {
                  if (enable) {
                    (void)m_pluginManager->enable(id);
                  } else {
                    m_pluginManager->disable(id);
                  }
                  markPluginListDirty();
                  requestSceneRebuild();
                },
            .isEnabling = [this](const std::string& id) { return m_pluginManager->isEnabling(id); },
            .addSource = [this]() { openPluginSourceCreateEditor(); },
            .setSourceEnabled =
                [this](PluginSourceConfig source, bool enabled) {
                  source.enabled = enabled;
                  m_pluginManager->addSource(source);
                  markPluginListDirty();
                  requestSceneRebuild();
                },
            .editSource = [this](PluginSourceConfig source) { openPluginSourceCreateEditor(std::move(source)); },
            .updateSource = [this](std::string source) { m_pluginManager->update(std::move(source)); },
            .refresh =
                [this]() {
                  markPluginListDirty();
                  requestSceneRebuild();
                },
            .autoUpdateEnabled = cfg.plugins.autoUpdate,
            .setAutoUpdate =
                [this](bool on) {
                  m_pluginManager->setAutoUpdateEnabled(on);
                  markPluginListDirty();
                  requestSceneRebuild();
                },
            .updateAll = [this]() { m_pluginManager->updateAll(); },
            .config = &cfg,
            .onConfigure = [this](std::string id) { openPluginSettingsEditor(std::move(id)); },
            .onRemove =
                [this](std::string id) {
                  if (m_pluginManager != nullptr) {
                    m_pluginManager->remove(id);
                    m_pendingDeletePluginId.clear();
                    markPluginListDirty();
                    requestSceneRebuild();
                  }
                },
            .openStore = [this]() { openPluginStore(); },
            .pendingDeletePluginId = m_pendingDeletePluginId,
            .requestDeleteConfirm =
                [this](std::string id) {
                  m_pendingDeletePluginId = std::move(id);
                  requestSceneRebuild();
                },
            .cancelDelete =
                [this]() {
                  m_pendingDeletePluginId.clear();
                  requestSceneRebuild();
                },
        }
    );
  }
  logSettingsProfile("rebuildContent plugins", phaseProfileWatch);
  logSettingsProfile("rebuildContent total", totalProfileWatch);
  if (noctalia::profiling::enabled()) {
    kLog.info(
        "profile rebuildContent visibleEntries={} registrySize={} selectedSection=\"{}\" searchActive={}",
        visibleEntries, m_settingsRegistry.size(), m_selectedSection, !m_searchQuery.empty()
    );
  }
}

std::unique_ptr<Flex> SettingsWindow::buildHeaderRow(float scale) {
  return ui::row(
      {
          .align = FlexAlign::Center,
          .justify = FlexJustify::SpaceBetween,
          .gap = Style::spaceSm * scale,
      },
      ui::label({
          .text = i18n::tr("settings.window.title"),
          .fontSize = Style::fontSizeTitle * scale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .flexGrow = 1.0f,
      }),
      ui::button({
          .out = &m_actionsMenuButton,
          .glyph = "more-vertical",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { openActionsMenu(); },
          .configure = [](Button& button) { button.setTabStop(false); },
      }),
      ui::button({
          .glyph = "close",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this]() { close(); },
          .configure = [](Button& button) { button.setTabStop(false); },
      })
  );
}

std::unique_ptr<Flex> SettingsWindow::buildFilterRow(
    float scale, const std::string& resetPageScope, std::vector<std::vector<std::string>> resetPagePaths
) {
  const auto requestRebuild = [this]() { requestSceneRebuild(); };
  const auto clearOverrides = [this](std::vector<std::vector<std::string>> paths) {
    clearSettingOverrides(std::move(paths));
  };

  auto filters = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd * scale,
  });

  Input* searchInputPtr = nullptr;
  filters->addChild(
      ui::input({
          .out = &searchInputPtr,
          .value = m_searchQuery,
          .placeholder = i18n::tr("settings.window.search-placeholder"),
          .fontSize = Style::fontSizeBody * scale,
          .controlHeight = Style::controlHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .clearButtonEnabled = true,
          .width = 320.0f * scale,
          .height = Style::controlHeight * scale,
          .onChange = [this](const std::string& value) {
            const bool wasSearchActive = !m_searchQuery.empty();
            m_searchQuery = value;
            const bool searchActiveChanged = wasSearchActive != !m_searchQuery.empty();
            const bool hadPendingReset = !m_pendingResetPageScope.empty() || !m_pendingResetSettingPaths.empty();
            m_pendingResetPageScope.clear();
            m_pendingResetSettingPaths.clear();

            if (hadPendingReset || searchActiveChanged) {
              // Toggling between empty/non-empty changes surrounding chrome and recreates the
              // search input, so rebuild immediately and restore focus.
              m_searchDebounceTimer.stop();
              m_focusSearchOnRebuild = true;
              requestSceneRebuild();
            } else {
              // Typing/deleting within an active query only re-filters the content list. Coalesce
              // bursts (held backspace, fast typing) so each key repeat doesn't walk the registry.
              m_searchDebounceTimer.start(kSearchDebounceInterval, [this]() { requestContentRebuild(); });
            }
          },
      })
  );
  m_settingsSearchInput = searchInputPtr;
  if (searchInputPtr != nullptr && searchInputPtr->inputArea() != nullptr) {
    searchInputPtr->inputArea()->setTabFocusKey("settings.search");
  }
  filters->addChild(ui::spacer());

  static const bool translatorMode = SysUtils::isEnvFlagOn("NOCTALIA_TRANSLATOR");
  if (translatorMode) {
    auto enLabel =
        makeLabel("en", Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::Error), FontWeight::Normal);
    filters->addChild(std::move(enLabel));

    filters->addChild(
        ui::toggle({
            .checked = m_forceEnTranslation,
            .scale = scale,
            .onChange = [this, requestRebuild](bool value) {
              m_forceEnTranslation = value;
              if (value)
                i18n::Service::instance().setLanguage("en");
              else if (m_config != nullptr)
                i18n::Service::instance().setLanguage(m_config->config().shell.lang);
              requestRebuild();
            },
        })
    );
  }

  auto advancedLabel = makeLabel(
      i18n::tr("settings.badges.advanced"), Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant),
      FontWeight::Normal
  );
  filters->addChild(std::move(advancedLabel));

  filters->addChild(
      ui::toggle({
          .checked = m_showAdvanced,
          .scale = scale,
          .onChange = [this, requestRebuild](bool value) {
            if (m_config != nullptr && !m_config->setOverride({"shell", "settings_show_advanced"}, value)) {
              markSettingsWriteError(i18n::tr("settings.errors.write"));
              return;
            }
            m_showAdvanced = value;
            const bool hadPendingReset = !m_pendingResetPageScope.empty() || !m_pendingResetSettingPaths.empty();
            m_pendingResetPageScope.clear();
            m_pendingResetSettingPaths.clear();
            if (hadPendingReset) {
              requestRebuild();
            } else {
              requestContentRebuild();
            }
          },
      })
  );

  auto overriddenLabel = makeLabel(
      i18n::tr("settings.window.filter-overridden"), Style::fontSizeBody * scale,
      colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
  );
  filters->addChild(std::move(overriddenLabel));

  filters->addChild(
      ui::toggle({
          .checked = m_showOverriddenOnly,
          .scale = scale,
          .onChange = [this, requestRebuild](bool value) {
            m_showOverriddenOnly = value;
            const bool hadPendingReset = !m_pendingResetPageScope.empty() || !m_pendingResetSettingPaths.empty();
            m_pendingResetPageScope.clear();
            m_pendingResetSettingPaths.clear();
            if (hadPendingReset) {
              requestRebuild();
            } else {
              requestContentRebuild();
            }
          },
      })
  );

  if (!resetPagePaths.empty()) {
    const bool pendingReset = m_pendingResetPageScope == resetPageScope;
    filters->addChild(
        ui::button({
            .text =
                pendingReset ? i18n::tr("settings.window.reset-page-confirm") : i18n::tr("settings.window.reset-page"),
            .fontSize = Style::fontSizeCaption * scale,
            .variant = pendingReset ? ButtonVariant::Destructive : ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * scale,
            .paddingV = Style::spaceXs * scale,
            .paddingH = Style::spaceSm * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this, resetPageScope, resetPagePaths = std::move(resetPagePaths), requestRebuild,
                        clearOverrides, pendingReset]() mutable {
              if (!pendingReset) {
                m_pendingResetSettingPaths.clear();
                m_pendingResetPageScope = resetPageScope;
                requestRebuild();
                return;
              }
              clearOverrides(std::move(resetPagePaths));
            },
        })
    );
  }

  return filters;
}

std::unique_ptr<Flex> SettingsWindow::buildStatusRow(float scale) {
  const auto* legacyIssue = m_config != nullptr && !m_config->legacyConfigIssues().empty()
      ? &m_config->legacyConfigIssues().front()
      : nullptr;
  if (m_statusMessage.empty() && legacyIssue == nullptr) {
    return nullptr;
  }

  const bool transientStatus = !m_statusMessage.empty();
  const bool statusIsError = transientStatus ? m_statusIsError : true;
  const std::string messageText = transientStatus
      ? m_statusMessage
      : i18n::tr("settings.window.legacy-config-warning", "issue", legacyIssue->path + ": " + legacyIssue->message);

  return settings::makeSettingsStatusBanner({
      .message = messageText,
      .error = statusIsError,
      .scale = scale,
      .onDismiss = transientStatus ? std::function<void()>{[this]() {
        clearStatusMessage();
        requestSceneRebuild();
      }}
                                   : std::function<void()>{},
  });
}

std::unique_ptr<Flex> SettingsWindow::buildBody(
    float scale, const Config& cfg, const std::vector<settings::SettingsSection>& sections,
    const std::vector<std::string>& availableBars
) {
  const auto requestRebuild = [this]() { requestSceneRebuild(); };
  const auto createBar = [this](std::string name) { this->createBar(std::move(name)); };
  const auto createMonitorOverride = [this](std::string barName, std::string match) {
    this->createMonitorOverride(std::move(barName), std::move(match));
  };
  const auto clearTransientSettingsState = [this]() { this->clearTransientSettingsState(); };
  const auto clearSearchQuery = [this]() { m_searchQuery.clear(); };

  auto body = ui::row({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  auto sidebar = settings::buildSettingsSidebar(
      settings::SettingsSidebarContext{
          .config = cfg,
          .sections = sections,
          .availableBars = availableBars,
          .scale = scale,
          .globalSearchActive = !m_searchQuery.empty(),
          .sidebarScrollState = m_sidebarScrollState,
          .contentScrollState = m_contentScrollState,
          .selectedSection = m_selectedSection,
          .selectedBarName = m_selectedBarName,
          .selectedMonitorOverride = m_selectedMonitorOverride,
          .creatingBarName = m_creatingBarName,
          .creatingMonitorOverrideBarName = m_creatingMonitorOverrideBarName,
          .creatingMonitorOverrideMatch = m_creatingMonitorOverrideMatch,
          .clearTransientState = clearTransientSettingsState,
          .clearSearchQuery = clearSearchQuery,
          .requestRebuild = requestRebuild,
          .createBar = createBar,
          .createMonitorOverride = createMonitorOverride,
          .scrollSidebarNodeIntoView = [this](const Node* node) { scrollSidebarNodeIntoView(node); },
          .outNav = &m_sidebarNav,
      }
  );
  m_sidebarScrollView = dynamic_cast<ScrollView*>(sidebar.get());

  body->addChild(std::move(sidebar));
  body->addChild(ui::separator());

  auto scroll = ui::scrollView({
      .out = &m_contentScrollView,
      .state = &m_contentScrollState,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0f,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0f,
      .onScrollChanged = [this](float /*offset*/) { dismissOpenSelectDropdown(); },
      .configure =
          [](ScrollView& scrollView) {
            scrollView.clearFill();
            scrollView.clearBorder();
          },
  });

  auto* content = m_contentScrollView->content();
  m_contentContainer = content;
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(Style::spaceMd * scale);
  rebuildSettingsContent();

  body->addChild(std::move(scroll));
  return body;
}

void SettingsWindow::refreshSettingsRegistry(const Config& cfg) {
  SettingsProfileWatch phaseProfileWatch;

  const auto env = buildRegistryEnvironment();
  logSettingsProfile("refreshRegistry registryEnvironment", phaseProfileWatch);
  phaseProfileWatch.reset();
  m_settingsRegistry = settings::buildSettingsRegistry(cfg, nullptr, nullptr, env);
  logSettingsProfile("refreshRegistry registry", phaseProfileWatch);
  phaseProfileWatch.reset();

  if (m_calendarService != nullptr
      && (m_calendarService->credentialMigrationPending()
          || m_calendarService->credentialState() != calendar::CredentialState::Ready)) {
    std::string descriptionKey = "settings.schema.services.calendar-credentials.description-error";
    switch (m_calendarService->credentialState()) {
    case calendar::CredentialState::Opening:
      descriptionKey = "settings.schema.services.calendar-credentials.description-opening";
      break;
    case calendar::CredentialState::Unavailable:
      descriptionKey = "settings.schema.services.calendar-credentials.description-unavailable";
      break;
    case calendar::CredentialState::Cancelled:
      descriptionKey = "settings.schema.services.calendar-credentials.description-cancelled";
      break;
    case calendar::CredentialState::DeniedOrLocked:
      descriptionKey = "settings.schema.services.calendar-credentials.description-locked";
      break;
    case calendar::CredentialState::BackendError:
    case calendar::CredentialState::Ready:
      break;
    }
    if (m_calendarService->credentialMigrationPending()
        && (m_calendarService->credentialState() == calendar::CredentialState::Opening
            || m_calendarService->credentialState() == calendar::CredentialState::Ready)) {
      descriptionKey = "settings.schema.services.calendar-credentials.description-migration";
    }

    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& entry) {
      return entry.section == settings::SettingsSection::Services && entry.group == "calendar";
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry retry{
        .section = settings::SettingsSection::Services,
        .group = "calendar",
        .title = i18n::tr("settings.schema.services.calendar-credentials.label"),
        .subtitle = i18n::tr(descriptionKey),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.services.calendar-credentials.button"),
                .action = [this]() { m_calendarService->retryCredentialMigration(); },
                .glyph = "refresh",
            },
        .searchText = "calendar credentials keyring secret service retry migration unlock",
    };
    m_settingsRegistry.insert(it, std::move(retry));
  }

  if (m_syncGreeterAppearance && env.greeterSyncAvailable) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Security
          && e.group == "greeter"
          && e.path == std::vector<std::string>{"shell", "greeter_sync", "privilege_command"};
    });
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Security,
        .group = "greeter",
        .title = i18n::tr("settings.schema.shell.sync-greeter.label"),
        .subtitle = i18n::tr("settings.schema.shell.sync-greeter.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.shell.sync-greeter.button"),
                .action = m_syncGreeterAppearance,
                .glyph = {},
            },
        .searchText = "greeter login sync appearance wallpaper colors security",
    };
    auto insertedIt = m_settingsRegistry.insert(it, std::move(btn));
    ++insertedIt;
    settings::SettingEntry toggle{
        .section = settings::SettingsSection::Security,
        .group = "greeter",
        .title = i18n::tr("settings.schema.shell.greeter-sync-auto.label"),
        .subtitle = i18n::tr("settings.schema.shell.greeter-sync-auto.description"),
        .path = {"shell", "greeter_sync", "auto_sync"},
        .control = settings::ToggleSetting{cfg.shell.greeterSync.autoSync},
        .searchText = "greeter sync auto automatic",
    };
    m_settingsRegistry.insert(insertedIt, std::move(toggle));
  }

  if (m_resetLauncherUsage) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Launcher
          && e.group == "launcher"
          && e.path == std::vector<std::string>{"shell", "launcher", "sort_by_usage"};
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Launcher,
        .group = "launcher",
        .title = i18n::tr("settings.schema.panels.launcher-reset-usage.label"),
        .subtitle = i18n::tr("settings.schema.panels.launcher-reset-usage.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.panels.launcher-reset-usage.button"),
                .action = m_resetLauncherUsage,
                .glyph = "refresh",
            },
        .searchText = "launcher reset usage recently used launch count history clear",
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_resetScreenTime) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::System
          && e.group == "screen-time"
          && e.path == std::vector<std::string>{"shell", "screen_time_enabled"};
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry btn{
        .section = settings::SettingsSection::System,
        .group = "screen-time",
        .title = i18n::tr("settings.schema.shell.screen-time-reset.label"),
        .subtitle = i18n::tr("settings.schema.shell.screen-time-reset.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.shell.screen-time-reset.button"),
                .action = m_resetScreenTime,
                .glyph = "refresh",
            },
        .searchText = "screen time reset usage history clear tracking",
        .visibleWhen = [](const Config& c) { return c.shell.screenTimeEnabled; },
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_saveWallpaperPaletteAsCustom && cfg.theme.source == PaletteSource::Wallpaper) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Appearance
          && e.group == "theme"
          && e.path == std::vector<std::string>{"theme", "wallpaper_scheme"};
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Appearance,
        .group = "theme",
        .title = i18n::tr("settings.schema.appearance.export-wallpaper-palette.label"),
        .subtitle = i18n::tr("settings.schema.appearance.export-wallpaper-palette.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.appearance.export-wallpaper-palette.button"),
                .action = m_saveWallpaperPaletteAsCustom,
                .glyph = {},
            },
        .searchText = "wallpaper palette export custom save colors theme",
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_openWallpaperPanel) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Wallpaper
          && e.group == "general"
          && e.path == std::vector<std::string>{"wallpaper", "fill_mode"};
    });
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Wallpaper,
        .group = "general",
        .title = i18n::tr("settings.schema.wallpaper.panel.label"),
        .subtitle = i18n::tr("settings.schema.wallpaper.panel.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.wallpaper.panel.button"),
                .action = m_openWallpaperPanel,
                .glyph = "wallpaper-selector"
            },
        .searchText = "wallpaper panel open selector browse",
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_openDesktopWidgetEditor) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Desktop && e.group == "widgets";
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Desktop,
        .group = "widgets",
        .title = i18n::tr("settings.schema.desktop.widgets-editor.label"),
        .subtitle = i18n::tr("settings.schema.desktop.widgets-editor.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.desktop.widgets-editor.button"),
                .action = m_openDesktopWidgetEditor,
                .glyph = {}
            },
        .searchText = "desktop widgets editor edit",
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_openLockscreenWidgetEditor) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Security
          && e.group == "lock-screen"
          && e.path == std::vector<std::string>{"lockscreen_widgets", "enabled"};
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    settings::SettingEntry btn{
        .section = settings::SettingsSection::Security,
        .group = "lock-screen",
        .title = i18n::tr("settings.schema.lockscreen.widgets-editor.label"),
        .subtitle = i18n::tr("settings.schema.lockscreen.widgets-editor.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.lockscreen.widgets-editor.button"),
                .action = m_openLockscreenWidgetEditor,
                .glyph = {}
            },
        .searchText = "lockscreen widgets editor edit layout",
        .visibleWhen = [](const Config& c) { return c.lockscreen.enabled && c.lockscreenWidgets.enabled; },
    };
    m_settingsRegistry.insert(it, std::move(btn));
  }

  if (m_config != nullptr) {
    auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Services
          && e.group == "calendar"
          && e.path == std::vector<std::string>{"calendar", "refresh_minutes"};
    });
    if (it != m_settingsRegistry.end()) {
      ++it;
    }
    const settings::SettingVisibility calendarOn = [](const Config& c) { return c.calendar.enabled; };
    settings::SettingEntry addBtn{
        .section = settings::SettingsSection::Services,
        .group = "calendar",
        .title = i18n::tr("settings.schema.services.calendar-add.label"),
        .subtitle = i18n::tr("settings.schema.services.calendar-add.description"),
        .path = {},
        .control =
            settings::ButtonSetting{
                .label = i18n::tr("settings.schema.services.calendar-add.button"),
                .action = [this]() { openCalendarAccountEditor(std::nullopt); },
                .glyph = "plus",
            },
        .searchText = "calendar add account icloud caldav google",
    };
    it = m_settingsRegistry.insert(it, std::move(addBtn));
    ++it;

    for (const CalendarConfig::Account& account : cfg.calendar.accounts) {
      if (account.type != "google" && account.type != "caldav") {
        continue;
      }
      settings::SettingEntry btn{
          .section = settings::SettingsSection::Services,
          .group = "calendar",
          .title = account.displayName.empty() ? account.id : account.displayName,
          .subtitle = i18n::tr("settings.schema.services.calendar-edit.description"),
          .path = {},
          .control =
              settings::ButtonSetting{
                  .label = i18n::tr("settings.schema.services.calendar-edit.button"),
                  .action = [this, id = account.id]() { openCalendarAccountEditor(id); },
                  .glyph = "edit",
              },
          .searchText = "calendar account edit connect authorize caldav icloud google password " + account.id,
          .visibleWhen = calendarOn,
      };
      it = m_settingsRegistry.insert(it, std::move(btn));
      ++it;
    }
  }
  logSettingsProfile("refreshRegistry injectedEntries", phaseProfileWatch);
}

std::vector<std::vector<std::string>> SettingsWindow::currentPageResetPaths() const {
  std::vector<std::vector<std::string>> resetPagePaths;
  if (m_config == nullptr) {
    return resetPagePaths;
  }
  for (const auto& entry : m_settingsRegistry) {
    if (!settingEntryBelongsToPage(entry, m_selectedSection, m_selectedBarName, m_selectedMonitorOverride)) {
      continue;
    }

    const auto appendIfOverridden = [this, &resetPagePaths](const std::vector<std::string>& path) {
      if (!path.empty() && m_config->hasEffectiveOverride(path) && !containsPath(resetPagePaths, path)) {
        resetPagePaths.push_back(path);
      }
    };
    appendIfOverridden(entry.path);
    if (const auto* range = std::get_if<settings::RangeSliderSetting>(&entry.control)) {
      appendIfOverridden(range->highPath);
    }
    if (const auto* select = std::get_if<settings::SelectSetting>(&entry.control)) {
      appendIfOverridden(select->linkedPath);
    }
  }
  return resetPagePaths;
}

bool SettingsWindow::tryPatchSettingsRegistryValue(
    const std::vector<std::string>& path, const ConfigOverrideValue& value
) {
  std::vector<settings::SettingEntry> patchedRegistry = m_settingsRegistry;
  if (!patchSettingsRegistryValue(patchedRegistry, path, value)) {
    return false;
  }
  m_settingsRegistry = std::move(patchedRegistry);
  return true;
}

bool SettingsWindow::tryPatchSettingsRegistryOverrides(
    const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides
) {
  std::vector<settings::SettingEntry> patchedRegistry = m_settingsRegistry;
  for (const auto& [path, value] : overrides) {
    if (!patchSettingsRegistryValue(patchedRegistry, path, value)) {
      return false;
    }
  }
  m_settingsRegistry = std::move(patchedRegistry);
  return true;
}

bool SettingsWindow::tryPatchSettingsRegistryResetValues(const std::vector<std::vector<std::string>>& paths) {
  if (m_config == nullptr) {
    return false;
  }

  std::vector<settings::SettingEntry> patchedRegistry = m_settingsRegistry;
  if (!patchSettingsRegistryResetValues(patchedRegistry, m_config->config(), paths)) {
    return false;
  }
  m_settingsRegistry = std::move(patchedRegistry);
  return true;
}

void SettingsWindow::rebuildFilterRow(float scale) {
  if (m_mainContainer == nullptr || m_filterRow == nullptr) {
    return;
  }

  const auto& children = m_mainContainer->children();
  auto it =
      std::ranges::find_if(children, [this](const std::unique_ptr<Node>& child) { return child.get() == m_filterRow; });
  if (it == children.end()) {
    return;
  }

  const auto index = static_cast<std::size_t>(std::distance(children.begin(), it));
  (void)m_mainContainer->removeChild(m_filterRow);
  const std::string resetPageScope = pageScopeKey(m_selectedSection, m_selectedBarName, m_selectedMonitorOverride);
  m_filterRow = m_mainContainer->insertChildAt(
      index, centeredRow(buildFilterRow(scale, resetPageScope, currentPageResetPaths()))
  );
}

void SettingsWindow::buildScene(std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("SettingsWindow::buildScene");
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  SettingsProfileWatch totalProfileWatch;
  SettingsProfileWatch phaseProfileWatch;

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const float scale = uiScale();
  m_actionsMenuButton = nullptr;
  m_contentScrollView = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarNav = nullptr;
  m_settingsSearchInput = nullptr;

  const Config fallbackCfg{};
  const Config& cfg = m_config != nullptr ? m_config->config() : fallbackCfg;
  const auto availableBars = settings::barNames(cfg);
  syncSelectedBarState(cfg, availableBars);

  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  logSettingsProfile("buildScene configSelection", phaseProfileWatch);
  phaseProfileWatch.reset();

  refreshSettingsRegistry(cfg);
  logSettingsProfile("buildScene refreshRegistry", phaseProfileWatch);
  phaseProfileWatch.reset();

  const auto sections = sectionKeys(m_settingsRegistry);
  const auto containsSection = [&sections](settings::SettingsSection section) {
    return std::ranges::contains(sections, section);
  };
  if (m_selectedSection == "bar" && selectedBar == nullptr) {
    m_selectedSection.clear();
  } else if (m_selectedSection != "bar" && !m_selectedSection.empty()) {
    const auto selectedSection = settings::settingsSectionFromId(m_selectedSection);
    if (!selectedSection.has_value() || !containsSection(*selectedSection)) {
      m_selectedSection.clear();
    }
  }
  if (m_selectedSection.empty()) {
    m_selectedSection = containsSection(settings::SettingsSection::Appearance)
        ? std::string(settings::settingsSectionId(settings::SettingsSection::Appearance))
        : (!sections.empty() ? std::string(settings::settingsSectionId(sections.front())) : std::string{});
  }

  const std::string resetPageScope = pageScopeKey(m_selectedSection, m_selectedBarName, m_selectedMonitorOverride);
  std::vector<std::vector<std::string>> resetPagePaths = currentPageResetPaths();
  if (m_pendingResetPageScope != resetPageScope) {
    m_pendingResetPageScope.clear();
  }
  logSettingsProfile("buildScene navigationState", phaseProfileWatch);
  phaseProfileWatch.reset();

  m_inputDispatcher.setSceneRoot(nullptr);
  m_mainContainer = nullptr;
  m_headerRow = nullptr;
  m_filterRow = nullptr;
  m_panelBackground = nullptr;
  m_contentContainer = nullptr;
  m_sceneRoot = std::make_unique<Node>();
  m_sceneRoot->setSize(w, h);
  m_sceneRoot->setAnimationManager(&m_animations);
  if (m_surface != nullptr && m_renderContext != nullptr && m_wayland != nullptr) {
    m_selectPopup = std::make_unique<SelectDropdownPopup>(*m_wayland, *m_renderContext);
    m_selectPopup->setShadowConfig(cfg.shell.shadow);
    m_selectPopup->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), m_output);
    m_sceneRoot->setPopupContext(m_selectPopup.get());
  }

  auto bg = ui::box({
      .width = w,
      .height = h,
      .configure = [](Box& box) {
        box.setPanelStyle();
        box.setRadius(0.0f);
        box.setBorder(clearColor(), 0);
        box.setPosition(0.0f, 0.0f);
      },
  });
  m_panelBackground = static_cast<Box*>(m_sceneRoot->addChild(std::move(bg)));
  logSettingsProfile("buildScene sceneRoot", phaseProfileWatch);
  phaseProfileWatch.reset();

  auto main = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::Start,
      .gap = Style::spaceMd * scale,
      .padding = Style::spaceLg * scale,
      .width = w,
      .height = h,
  });

  m_headerRow = main->addChild(centeredRow(buildHeaderRow(scale)));
  m_filterRow = main->addChild(centeredRow(buildFilterRow(scale, resetPageScope, std::move(resetPagePaths))));
  if (auto status = buildStatusRow(scale)) {
    main->addChild(centeredRow(std::move(status)));
  }
  logSettingsProfile("buildScene chrome", phaseProfileWatch);
  phaseProfileWatch.reset();

  auto bodyRow = centeredRow(buildBody(scale, cfg, sections, availableBars));
  bodyRow->setFlexGrow(1.0f);
  main->addChild(std::move(bodyRow));
  logSettingsProfile("buildScene body", phaseProfileWatch);
  phaseProfileWatch.reset();

  main->setSize(w, h);
  main->layout(*m_renderContext);
  logSettingsProfile("buildScene layout", phaseProfileWatch);
  phaseProfileWatch.reset();
  applyPendingContentScrollTarget(Style::spaceMd * scale);
  logSettingsProfile("buildScene scrollTarget", phaseProfileWatch);
  phaseProfileWatch.reset();
  m_mainContainer = static_cast<Flex*>(m_sceneRoot->addChild(std::move(main)));

  m_inputDispatcher.setTextInputContext(m_surface->wlSurface(), m_wayland->textInputService());
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });
  m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
    if (m_surface != nullptr) {
      wl_output* output = m_output;
      if (output == nullptr && m_wayland != nullptr) {
        output = m_wayland->outputForSurface(m_surface->wlSurface());
      }
      TooltipManager::instance().onHoverChange(next, m_surface->xdgSurface(), output);
    }
  });
  m_inputDispatcher.setFocusChangeCallback([this](InputArea* /*old*/, InputArea* next) {
    scrollFocusedAreaIntoView(next);
  });
  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  m_surface->setSceneRoot(m_sceneRoot.get());
  logSettingsProfile("buildScene input", phaseProfileWatch);
  logSettingsProfile("buildScene total", totalProfileWatch);
  if (noctalia::profiling::enabled()) {
    kLog.info(
        "profile buildScene registrySize={} sections={} selectedSection=\"{}\" size={}x{}", m_settingsRegistry.size(),
        sections.size(), m_selectedSection, width, height
    );
  }
}
