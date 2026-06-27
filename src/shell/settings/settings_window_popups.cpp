#include "config/atomic_file.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "notification/notification_filter.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/bar_widget_editor.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_content_plugins.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_window.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/flex.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/popup_parent.h"
#include "util/string_utils.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr std::int32_t kActionSupportReport = 1;
  constexpr std::int32_t kActionExportConfig = 2;
  constexpr std::string_view kCalendarCredentialOwner = "calendar_credentials";

  XdgPopupParent popupParentFor(ToplevelSurface& surface, wl_output* output, std::uint32_t serial) {
    return XdgPopupParent{
        .xdgSurface = surface.xdgSurface(),
        .wlSurface = surface.wlSurface(),
        .output = output,
        .serial = serial,
        .width = surface.width(),
        .height = surface.height(),
    };
  }

  struct PluginSourceDraft {
    PluginSourceKind kind = PluginSourceKind::Git;
    std::string name;
    std::string location;
    bool autoUpdate = false;
    bool enabled = true;
    bool editing = false;
    bool nameInvalid = false;
    bool locationInvalid = false;
    std::string error;
  };

  enum class CalendarAccountProvider : std::uint8_t {
    ICloud,
    CustomCalDav,
    Google,
  };

  struct CalendarAccountDraft {
    bool creating = true;
    CalendarAccountProvider provider = CalendarAccountProvider::ICloud;
    std::string id = "personal_icloud";
    std::string name;
    std::string username;
    std::string password;
    std::string serverUrl;
    std::string color;
    bool idInvalid = false;
    bool usernameInvalid = false;
    bool passwordInvalid = false;
    bool serverUrlInvalid = false;
  };

  bool validCalendarAccountId(std::string_view id) {
    if (id.empty()) {
      return false;
    }
    return std::ranges::all_of(id, [](char ch) {
      return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
  }

  bool calendarAccountIdExists(const Config& cfg, std::string_view id) {
    return std::ranges::contains(cfg.calendar.accounts, id, &CalendarConfig::Account::id);
  }

  bool pluginSourceNameExists(const Config& cfg, std::string_view name) {
    return std::ranges::contains(cfg.plugins.sources, name, &PluginSourceConfig::name);
  }

  std::size_t pluginSourceKindIndex(PluginSourceKind kind) { return kind == PluginSourceKind::Path ? 1u : 0u; }

  const CalendarConfig::Account* findCalendarAccount(const Config& cfg, std::string_view id) {
    const auto it = std::ranges::find(cfg.calendar.accounts, id, &CalendarConfig::Account::id);
    return it != cfg.calendar.accounts.end() ? &*it : nullptr;
  }

  std::string calendarProviderKey(CalendarAccountProvider provider) {
    switch (provider) {
    case CalendarAccountProvider::ICloud:
      return "icloud";
    case CalendarAccountProvider::CustomCalDav:
      return "custom";
    case CalendarAccountProvider::Google:
      return "google";
    }
    return "icloud";
  }

  std::string calendarProviderTitle(CalendarAccountProvider provider) {
    switch (provider) {
    case CalendarAccountProvider::ICloud:
      return i18n::tr("settings.calendar-accounts.provider.icloud");
    case CalendarAccountProvider::CustomCalDav:
      return i18n::tr("settings.calendar-accounts.provider.custom");
    case CalendarAccountProvider::Google:
      return i18n::tr("settings.calendar-accounts.provider.google");
    }
    return i18n::tr("settings.calendar-accounts.provider.icloud");
  }

  std::string trimInput(Input* input) { return input != nullptr ? StringUtils::trim(input->value()) : std::string{}; }

  std::string sessionActionTitle(const SessionPanelActionConfig& row) {
    return settings::sessionActionDisplayTitle(row);
  }

  std::string idleBehaviorTitle(const IdleBehaviorConfig& row) {
    IdleBehaviorConfig norm = row;
    normalizeIdleBehaviorAction(norm);
    if (norm.action == "lock") {
      return i18n::tr("settings.idle.behavior.kind.lock");
    }
    if (norm.action == "screen_off") {
      return i18n::tr("settings.idle.behavior.kind.screen-off");
    }
    if (norm.action == "suspend") {
      return i18n::tr("settings.idle.behavior.kind.suspend");
    }
    if (norm.action == "lock_and_suspend") {
      return i18n::tr("settings.idle.behavior.kind.lock-and-suspend");
    }
    if (!StringUtils::trim(row.name).empty()) {
      return row.name;
    }
    return i18n::tr("settings.idle.behavior.unnamed");
  }

  std::string notificationFilterTitle(const NotificationFilterConfig& row) {
    if (!row.match.empty()) {
      return row.match;
    }
    if (!StringUtils::trim(row.name).empty()) {
      return row.name;
    }
    return i18n::tr("settings.notifications.filter.unnamed");
  }

  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows) {
    std::vector<std::string> used;
    used.reserve(rows.size());
    for (auto& row : rows) {
      std::string base = StringUtils::trim(row.name);
      if (base.empty()) {
        base = "idle-behavior";
      }
      for (char& ch : base) {
        if (ch == '.' || ch == '[' || ch == ']') {
          ch = '-';
        }
      }

      std::string candidate = base;
      for (int suffix = 2; std::ranges::contains(used, candidate); ++suffix) {
        candidate = std::format("{}-{}", base, suffix);
      }
      row.name = candidate;
      used.push_back(row.name);
    }
  }

  bool isBarWidgetListPath(const std::vector<std::string>& path) {
    if (path.size() < 3 || path.front() != "bar") {
      return false;
    }
    const auto& key = path.back();
    return key == "start" || key == "center" || key == "end";
  }

  std::vector<std::string> barWidgetItemsForPath(const Config& cfg, const std::vector<std::string>& path) {
    if (!isBarWidgetListPath(path) || path.size() < 3) {
      return {};
    }

    const auto* bar = settings::findBar(cfg, path[1]);
    if (bar == nullptr) {
      return {};
    }

    const auto& lane = path.back();
    if (path.size() >= 5 && path[2] == "monitor") {
      const auto* ovr = settings::findMonitorOverride(*bar, path[3]);
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

} // namespace

void SettingsWindow::openActionsMenu() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_actionsMenuButton == nullptr
      || m_surface->xdgSurface() == nullptr) {
    return;
  }

  if (m_actionsMenuPopup == nullptr) {
    m_actionsMenuPopup = std::make_unique<ContextMenuPopup>(*m_wayland, *m_renderContext);
    m_actionsMenuPopup->setOnActivate([this](const ContextMenuControlEntry& entry) {
      switch (entry.id) {
      case kActionSupportReport:
        if (m_actionsMenuPopup != nullptr) {
          m_actionsMenuPopup->close();
        }
        DeferredCall::callLater([this]() { saveSupportReport(); });
        break;
      case kActionExportConfig:
        if (m_actionsMenuPopup != nullptr) {
          m_actionsMenuPopup->close();
        }
        DeferredCall::callLater([this]() { openConfigExportDialog(); });
        break;
      default:
        break;
      }
    });
  } else if (m_actionsMenuPopup->isOpen()) {
    m_actionsMenuPopup->close();
    return;
  }

  std::vector<ContextMenuControlEntry> entries;
  entries.push_back(
      {.id = kActionSupportReport,
       .label = i18n::tr("settings.window.support-report"),
       .enabled = true,
       .separator = false,
       .hasSubmenu = false}
  );
  entries.push_back(
      {.id = kActionExportConfig,
       .label = i18n::tr("settings.window.export-config"),
       .enabled = true,
       .separator = false,
       .hasSubmenu = false}
  );

  float anchorAbsX = 0.0f;
  float anchorAbsY = 0.0f;
  Node::absolutePosition(m_actionsMenuButton, anchorAbsX, anchorAbsY);

  const float scale = uiScale();
  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  if (m_config != nullptr) {
    m_actionsMenuPopup->setShadowConfig(m_config->config().shell.shadow);
  }
  m_actionsMenuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .menuWidth = 220.0f * scale,
          .maxVisible = 8,
          .anchor =
              PopupAnchorRect{
                  .x = static_cast<std::int32_t>(anchorAbsX),
                  .y = static_cast<std::int32_t>(anchorAbsY),
                  .width = static_cast<std::int32_t>(m_actionsMenuButton->width()),
                  .height = static_cast<std::int32_t>(m_actionsMenuButton->height()),
              },
          .parent = PopupSurfaceParent{
              .xdgSurface = m_surface->xdgSurface(),
              .output = output,
          },
      }
  );
}

void SettingsWindow::openConfigExportDialog() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_configExportDialogPopup == nullptr) {
    m_configExportDialogPopup = std::make_unique<settings::ConfigExportDialogPopup>();
    m_configExportDialogPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_configExportDialogPopup->open(
      settings::ConfigExportDialogPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .scale = uiScale(),
          .callback = [this](settings::ConfigExportMode mode) { saveConfigExport(mode); },
      }
  );
}

void SettingsWindow::openBarWidgetAddPopup(const std::vector<std::string>& lanePath) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }

  if (m_widgetAddPopup == nullptr) {
    m_widgetAddPopup = std::make_unique<settings::WidgetAddPopup>();
    m_widgetAddPopup->initialize(*m_wayland, *m_config, *m_renderContext);
    m_widgetAddPopup->setOnSelect([this](
                                      const std::vector<std::string>& selectedLanePath, const std::string& value,
                                      const std::string& newInstanceType, const std::string& newInstanceId,
                                      const std::vector<std::pair<std::string, std::string>>& initialSettings
                                  ) {
      if (value.empty() || m_config == nullptr) {
        return;
      }

      const Config& activeConfig = m_config->config();
      auto laneItems = barWidgetItemsForPath(activeConfig, selectedLanePath);

      m_pendingDeleteWidgetName.clear();
      m_pendingDeleteWidgetSettingPath.clear();
      m_renamingWidgetName.clear();
      m_editingWidgetName.clear();

      if (!newInstanceType.empty() && !newInstanceId.empty()) {
        laneItems.push_back(newInstanceId);
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides = {
            {{"widget", newInstanceId, "type"}, newInstanceType},
        };
        for (const auto& [key, settingValue] : initialSettings) {
          overrides.push_back({{"widget", newInstanceId, key}, settingValue});
        }
        overrides.emplace_back(selectedLanePath, laneItems);
        setSettingOverrides(overrides);
        return;
      }

      laneItems.push_back(value);
      setSettingOverride(selectedLanePath, laneItems);
    });
  }

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_widgetAddPopup->open(
      settings::WidgetAddPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .lanePath = lanePath,
          .config = m_config->config(),
          .scale = uiScale(),
      }
  );
}

void SettingsWindow::openSearchPickerPopup(settings::SearchPickerOpenRequest request) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr
      || request.options.empty()) {
    return;
  }

  if (m_searchPickerPopup == nullptr) {
    m_searchPickerPopup = std::make_unique<settings::SearchPickerPopup>();
    m_searchPickerPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }

  m_searchPickerPopup->setOnSelect([this, settingPath = request.settingPath,
                                    selectedValue = request.selectedValue](const std::string& value) {
    if (value == selectedValue) {
      return;
    }
    if (value.empty()) {
      clearSettingOverride(settingPath);
      return;
    }
    setSettingOverride(settingPath, value);
  });

  std::vector<SearchPickerOption> pickerOptions;
  pickerOptions.reserve(request.options.size());
  for (const auto& opt : request.options) {
    pickerOptions.push_back(
        SearchPickerOption{
            .value = opt.value,
            .label = opt.label,
            .description = opt.description,
            .enabled = true,
            .icon = {},
            .preview = opt.preview,
        }
    );
  }

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  XdgPopupParent parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial());
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    parent.xdgSurface = m_editorSheetPopup->xdgSurface();
    parent.wlSurface = m_editorSheetPopup->wlSurface();
    parent.width = m_editorSheetPopup->width();
    parent.height = m_editorSheetPopup->height();
  }

  m_searchPickerPopup->open(
      settings::SearchPickerPopupRequest{
          .parent = parent,
          .title = std::move(request.title),
          .options = std::move(pickerOptions),
          .selectedValue = std::move(request.selectedValue),
          .placeholder = std::move(request.placeholder),
          .emptyText = std::move(request.emptyText),
          .scale = uiScale(),
      }
  );
}

void SettingsWindow::openSessionActionEntryEditor(std::size_t index) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  const Config& cfg = m_config->config();
  if (index >= cfg.shell.session.actions.size()) {
    return;
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<SessionPanelActionConfig>(cfg.shell.session.actions[index]);

  const auto persist = [this, rowState, index]() {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().shell.session.actions;
    if (index >= next.size()) {
      return;
    }
    next[index] = *rowState;
    if (m_sessionActionsEditState != nullptr && index < m_sessionActionsEditState->size()) {
      (*m_sessionActionsEditState)[index] = *rowState;
    }
    syncSessionActionInlineSummary(index, *rowState);
    setSettingOverride({"shell", "session", "actions"}, next);
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->setSheetTitle(sessionActionTitle(*rowState));
      m_editorSheetPopup->requestLayout();
    }
  };

  const auto removeRow = [this, index]() {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().shell.session.actions;
    if (index >= next.size()) {
      return;
    }
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    setSettingOverride({"shell", "session", "actions"}, next);
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
    requestContentRebuild();
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.closeHostedEditor = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
  };

  const std::string sheetTitle = sessionActionTitle(*rowState);

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = sheetTitle,
          .removeAction = removeRow,
          .populateSheetBody =
              [ctx, rowState, persist](Flex& body) mutable {
                settings::buildSessionActionEntryDetailContent(body, ctx, *rowState, persist);
              },
          .scale = scale,
      }
  );
}

void SettingsWindow::openIdleBehaviorEntryEditor(std::size_t index) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  // Closing the previous hosted editor can commit focused fields via focus-loss callbacks.
  // Do it before reading cfg/rowState so the new editor is built from the latest config.
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }

  const Config& cfg = m_config->config();
  if (index >= cfg.idle.behaviors.size()) {
    return;
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<IdleBehaviorConfig>(cfg.idle.behaviors[index]);
  auto rowKey = std::make_shared<std::string>(rowState->name);
  normalizeIdleBehaviorAction(*rowState);

  const auto persist = [this, rowState, rowKey, index]() {
    if (m_config == nullptr) {
      return;
    }
    normalizeIdleBehaviorAction(*rowState);
    auto next = m_config->config().idle.behaviors;
    auto target = std::ranges::find(next, *rowKey, &IdleBehaviorConfig::name);
    if (target == next.end() && index < next.size()) {
      target = next.begin() + static_cast<std::ptrdiff_t>(index);
    }
    if (target == next.end()) {
      return;
    }
    const auto targetIndex = static_cast<std::size_t>(std::distance(next.begin(), target));
    next[targetIndex] = *rowState;
    normalizeIdleBehaviorNames(next);
    *rowState = next[targetIndex];
    *rowKey = rowState->name;
    setSettingOverride({"idle", "behavior"}, next);
    requestContentRebuild();
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestLayout();
    }
  };

  const auto removeRow = [this, index]() {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().idle.behaviors;
    if (index >= next.size()) {
      return;
    }
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    normalizeIdleBehaviorNames(next);
    setSettingOverride({"idle", "behavior"}, next);
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
    requestContentRebuild();
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.closeHostedEditor = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = idleBehaviorTitle(*rowState),
          .removeAction = removeRow,
          .populateSheetBody =
              [ctx, rowState, persist](Flex& body) mutable {
                settings::buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persist);
              },
          .scale = scale,
      }
  );
}

void SettingsWindow::openIdleBehaviorCreateEditor() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  const Config& cfg = m_config->config();
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<IdleBehaviorConfig>(IdleBehaviorConfig{
      .name = "idle-behavior",
      .enabled = false,
      .timeoutSeconds = 600,
      .action = "command",
      .command = "",
      .resumeCommand = "",
  });

  const auto persistDraft = [this]() {
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestLayout();
    }
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.afterIdleBehaviorApply = [this, rowState]() {
    if (m_config == nullptr) {
      return;
    }
    normalizeIdleBehaviorAction(*rowState);
    auto next = m_config->config().idle.behaviors;
    next.push_back(*rowState);
    normalizeIdleBehaviorNames(next);
    setSettingOverride({"idle", "behavior"}, next);
    requestContentRebuild();
  };
  ctx.closeHostedEditor = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = idleBehaviorTitle(*rowState),
          .removeAction = nullptr,
          .populateSheetBody =
              [ctx, rowState, persistDraft](Flex& body) mutable {
                settings::buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persistDraft);
              },
          .scale = scale,
      }
  );
}

void SettingsWindow::openNotificationFilterEntryEditor(std::size_t index) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }

  const Config& cfg = m_config->config();
  if (index >= cfg.notification.filters.size()) {
    return;
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<NotificationFilterConfig>(cfg.notification.filters[index]);
  auto rowKey = std::make_shared<std::string>(rowState->name);

  const auto persist = [this, rowState, rowKey, index]() {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().notification.filters;
    auto target = std::ranges::find(next, *rowKey, &NotificationFilterConfig::name);
    if (target == next.end() && index < next.size()) {
      target = next.begin() + static_cast<std::ptrdiff_t>(index);
    }
    if (target == next.end()) {
      return;
    }
    const auto targetIndex = static_cast<std::size_t>(std::distance(next.begin(), target));
    next[targetIndex] = *rowState;
    normalizeNotificationFilterNames(next);
    *rowState = next[targetIndex];
    *rowKey = rowState->name;
    setSettingOverride({"notification", "filter"}, next);
    requestContentRebuild();
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestLayout();
    }
  };

  const auto removeRow = [this, index]() {
    if (m_config == nullptr) {
      return;
    }
    auto next = m_config->config().notification.filters;
    if (index >= next.size()) {
      return;
    }
    next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
    normalizeNotificationFilterNames(next);
    setSettingOverride({"notification", "filter"}, next);
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
    requestContentRebuild();
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openNotificationFilterEntryEditor = {};
  ctx.afterNotificationFilterApply = [persist]() { persist(); };
  ctx.closeHostedEditor = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = notificationFilterTitle(*rowState),
          .removeAction = removeRow,
          .populateSheetBody =
              [ctx, rowState, persist](Flex& body) mutable {
                settings::buildNotificationFilterEntryDetailContent(body, ctx, *rowState, persist);
              },
          .scale = scale,
      }
  );
}

void SettingsWindow::openNotificationFilterCreateEditor() {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  const Config& cfg = m_config->config();
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<NotificationFilterConfig>(NotificationFilterConfig{
      .name = "filter",
      .enabled = true,
      .match = {},
      .showToast = true,
      .saveHistory = true,
      .playSound = true,
      .allowPermanent = true,
      .allowedUrgencies = {},
  });

  const auto persistDraft = [this]() {
    if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
      m_editorSheetPopup->requestLayout();
    }
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openNotificationFilterEntryEditor = {};
  ctx.afterNotificationFilterApply = [this, rowState]() {
    if (m_config == nullptr || rowState->match.empty()) {
      return;
    }
    auto next = m_config->config().notification.filters;
    next.push_back(*rowState);
    normalizeNotificationFilterNames(next);
    setSettingOverride({"notification", "filter"}, next);
    requestContentRebuild();
  };
  ctx.closeHostedEditor = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = i18n::tr("settings.notifications.filter.add-title"),
          .removeAction = nullptr,
          .populateSheetBody =
              [ctx, rowState, persistDraft](Flex& body) mutable {
                settings::buildNotificationFilterEntryDetailContent(body, ctx, *rowState, persistDraft);
              },
          .scale = scale,
      }
  );
}

void SettingsWindow::openCalendarAccountEditor(std::optional<std::string> accountId) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  const Config& cfg = m_config->config();
  auto draft = std::make_shared<CalendarAccountDraft>();
  if (accountId.has_value()) {
    const CalendarConfig::Account* account = findCalendarAccount(cfg, *accountId);
    if (account == nullptr || (account->type != "caldav" && account->type != "google")) {
      return;
    }
    draft->creating = false;
    draft->id = account->id;
    draft->name = account->displayName;
    draft->username = account->username;
    draft->serverUrl = account->serverUrl;
    draft->color = account->color;
    if (account->type == "google") {
      draft->provider = CalendarAccountProvider::Google;
    } else {
      draft->provider =
          account->provider == "custom" ? CalendarAccountProvider::CustomCalDav : CalendarAccountProvider::ICloud;
    }
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  const float scale = uiScale();
  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  const std::string title = draft->creating ? i18n::tr("settings.calendar-accounts.add-title")
                                            : i18n::tr("settings.calendar-accounts.edit-title");

  std::function<void()> removeAccount;
  if (!draft->creating && m_config->isOverrideOnlyCalendarAccount(draft->id)) {
    removeAccount = [this, accountId = draft->id]() {
      if (m_config == nullptr) {
        return;
      }
      if (!m_config->deleteCalendarAccountOverride(accountId)) {
        markSettingsWriteError(i18n::tr("settings.calendar-accounts.delete-error"));
        return;
      }
      (void)m_config->setStateString(kCalendarCredentialOwner, accountId + "_password", "");
      (void)m_config->setStateString(kCalendarCredentialOwner, accountId + "_refresh_token", "");
      (void)m_config->setStateString(kCalendarCredentialOwner, accountId + "_access_token", "");
      (void)m_config->setStateString(kCalendarCredentialOwner, accountId + "_access_expiry", "");
      markSettingsWriteSuccess(true);
      if (m_editorSheetPopup != nullptr) {
        m_editorSheetPopup->close();
      }
    };
  }

  auto populateSheetBody = [this, draft, scale](Flex& body) mutable {
    auto addField = [scale](Flex& parent, const std::string& label, std::unique_ptr<Node> control) {
      auto field = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
      });
      field->addChild(
          ui::label({
              .text = label,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .fontWeight = FontWeight::Medium,
          })
      );
      field->addChild(std::move(control));
      parent.addChild(std::move(field));
    };

    const auto providerIndex = [](CalendarAccountProvider provider) -> std::size_t {
      switch (provider) {
      case CalendarAccountProvider::ICloud:
        return 0;
      case CalendarAccountProvider::CustomCalDav:
        return 1;
      case CalendarAccountProvider::Google:
        return 2;
      }
      return 0;
    };
    addField(
        body, i18n::tr("settings.calendar-accounts.provider-label"),
        ui::segmented({
            .options =
                std::vector<ui::SegmentedOption>{
                    {.label = calendarProviderTitle(CalendarAccountProvider::ICloud), .glyph = "brand-apple"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::CustomCalDav), .glyph = "calendar-cog"},
                    {.label = calendarProviderTitle(CalendarAccountProvider::Google), .glyph = "brand-google"},
                },
            .selectedIndex = providerIndex(draft->provider),
            .scale = scale,
            .enabled = draft->creating,
            .equalSegmentWidths = true,
            .onChange = [this, draft](std::size_t index) {
              CalendarAccountProvider provider = CalendarAccountProvider::ICloud;
              if (index == 1) {
                provider = CalendarAccountProvider::CustomCalDav;
              } else if (index == 2) {
                provider = CalendarAccountProvider::Google;
              }

              draft->provider = provider;
              if (provider == CalendarAccountProvider::Google && draft->id == "personal_icloud") {
                draft->id = "personal_google";
              } else if (provider == CalendarAccountProvider::CustomCalDav && draft->id == "personal_icloud") {
                draft->id = "home_nextcloud";
              } else if (provider == CalendarAccountProvider::ICloud && draft->id == "personal_google") {
                draft->id = "personal_icloud";
              }
              if (m_editorSheetPopup != nullptr) {
                m_editorSheetPopup->rebuildBody();
              }
            },
        })
    );

    Input* idInput = nullptr;
    addField(
        body, i18n::tr("settings.calendar-accounts.id-label"),
        ui::input({
            .out = &idInput,
            .value = draft->id,
            .placeholder = "personal_icloud",
            .invalid = draft->idInvalid,
            .enabled = draft->creating,
            .onChange = [draft](const std::string& value) {
              if (draft->creating) {
                draft->id = value;
              }
              draft->idInvalid = false;
            },
        })
    );

    Input* nameInput = nullptr;
    addField(
        body, i18n::tr("settings.calendar-accounts.name-label"),
        ui::input({
            .out = &nameInput,
            .value = draft->name,
            .placeholder = i18n::tr("settings.calendar-accounts.name-placeholder"),
            .onChange = [draft](const std::string& value) { draft->name = value; },
        })
    );

    Input* usernameInput = nullptr;
    Input* passwordInput = nullptr;
    Input* serverInput = nullptr;
    if (draft->provider != CalendarAccountProvider::Google) {
      addField(
          body, i18n::tr("settings.calendar-accounts.username-label"),
          ui::input({
              .out = &usernameInput,
              .value = draft->username,
              .placeholder = i18n::tr("settings.calendar-accounts.username-placeholder"),
              .invalid = draft->usernameInvalid,
              .onChange = [draft](const std::string& value) {
                draft->username = value;
                draft->usernameInvalid = false;
              },
          })
      );
      addField(
          body, i18n::tr("settings.calendar-accounts.password-label"),
          ui::input({
              .out = &passwordInput,
              .value = {},
              .placeholder = draft->creating ? i18n::tr("settings.calendar-accounts.password-placeholder")
                                             : i18n::tr("settings.calendar-accounts.password-keep-placeholder"),
              .passwordMode = true,
              .invalid = draft->passwordInvalid,
              .onChange = [draft](const std::string& value) {
                draft->password = value;
                draft->passwordInvalid = false;
              },
          })
      );
    }
    if (draft->provider == CalendarAccountProvider::CustomCalDav) {
      addField(
          body, i18n::tr("settings.calendar-accounts.server-url-label"),
          ui::input({
              .out = &serverInput,
              .value = draft->serverUrl,
              .placeholder = "https://cloud.example.com/remote.php/dav/",
              .invalid = draft->serverUrlInvalid,
              .onChange = [draft](const std::string& value) {
                draft->serverUrl = value;
                draft->serverUrlInvalid = false;
              },
          })
      );
    }

    addField(
        body, i18n::tr("settings.calendar-accounts.color-label"),
        settings::makeColorSpecSelect(
            settings::ColorSpecSelectOptions{
                .roles = {},
                .selectedValue = draft->color,
                .allowNone = true,
                .allowCustomColor = true,
                .noneLabel = {},
                .fontSize = Style::fontSizeBody * scale,
                .controlHeight = Style::controlHeight * scale,
                .glyphSize = Style::fontSizeBody * scale,
                .flexGrow = true,
            },
            [draft](std::string value) { draft->color = StringUtils::trim(value); }, [draft]() { draft->color.clear(); }
        )
    );

    const auto persistAccount = [this, draft, idInput, nameInput, usernameInput, passwordInput,
                                 serverInput](bool closeAfter, bool connectAfter) {
      if (m_config == nullptr) {
        return;
      }

      draft->id = draft->creating ? trimInput(idInput) : draft->id;
      draft->name = trimInput(nameInput);
      draft->color = StringUtils::trim(draft->color);
      draft->username = trimInput(usernameInput);
      draft->password = trimInput(passwordInput);
      draft->serverUrl = trimInput(serverInput);

      draft->idInvalid = false;
      draft->usernameInvalid = false;
      draft->passwordInvalid = false;
      draft->serverUrlInvalid = false;

      if (!validCalendarAccountId(draft->id)) {
        draft->idInvalid = true;
      }
      if (draft->creating && calendarAccountIdExists(m_config->config(), draft->id)) {
        draft->idInvalid = true;
      }

      const bool caldav = draft->provider != CalendarAccountProvider::Google;
      if (caldav && draft->username.empty()) {
        draft->usernameInvalid = true;
      }
      if (draft->provider == CalendarAccountProvider::CustomCalDav && draft->serverUrl.empty()) {
        draft->serverUrlInvalid = true;
      }
      if (caldav && draft->password.empty()) {
        const std::string existing =
            m_config->stateString(kCalendarCredentialOwner, draft->id + "_password").value_or(std::string{});
        if (existing.empty()) {
          draft->passwordInvalid = true;
        }
      }
      if (draft->idInvalid || draft->usernameInvalid || draft->passwordInvalid || draft->serverUrlInvalid) {
        showTransientStatus(i18n::tr("settings.calendar-accounts.invalid"), true);
        return;
      }

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      if (draft->creating) {
        overrides.push_back({{"calendar", "enabled"}, true});
      }
      const std::vector<std::string> base = {"calendar", "account", draft->id};
      overrides.push_back(
          {{base[0], base[1], base[2], "type"}, caldav ? std::string("caldav") : std::string("google")}
      );
      overrides.push_back({{base[0], base[1], base[2], "name"}, draft->name});
      overrides.push_back({{base[0], base[1], base[2], "color"}, draft->color});
      if (caldav) {
        overrides.push_back({{base[0], base[1], base[2], "provider"}, calendarProviderKey(draft->provider)});
        overrides.push_back({{base[0], base[1], base[2], "username"}, draft->username});
        if (draft->provider == CalendarAccountProvider::CustomCalDav) {
          overrides.push_back({{base[0], base[1], base[2], "server_url"}, draft->serverUrl});
        }
      }

      if (caldav && !draft->password.empty()) {
        if (!m_config->setStateString(kCalendarCredentialOwner, draft->id + "_password", draft->password)) {
          markSettingsWriteError(i18n::tr("settings.calendar-accounts.password-save-error"));
          return;
        }
      }

      if (!m_config->setOverrides(std::move(overrides))) {
        markSettingsWriteError(i18n::tr("settings.calendar-accounts.save-error"));
        return;
      }

      std::function<void(std::string, std::string)> connectCalendarAccount;
      std::string connectAccountId;
      std::string connectActivationToken;
      if (connectAfter && m_connectCalendarAccount) {
        connectCalendarAccount = m_connectCalendarAccount;
        connectAccountId = draft->id;
        if (m_wayland != nullptr && m_surface != nullptr) {
          connectActivationToken = m_wayland->requestActivationToken(m_surface->wlSurface());
        }
      }

      markSettingsWriteSuccess(closeAfter);
      if (connectCalendarAccount) {
        DeferredCall::callLater([connectCalendarAccount = std::move(connectCalendarAccount),
                                 connectAccountId = std::move(connectAccountId),
                                 connectActivationToken = std::move(connectActivationToken)]() mutable {
          connectCalendarAccount(connectAccountId, connectActivationToken);
        });
      }
      if (closeAfter && m_editorSheetPopup != nullptr) {
        m_editorSheetPopup->close();
      }
    };

    auto actions = ui::row({
        .align = FlexAlign::Center,
        .justify = FlexJustify::End,
        .gap = Style::spaceSm * scale,
    });
    actions->addChild(
        ui::button({
            .text = i18n::tr("common.actions.cancel"),
            .variant = ButtonVariant::Secondary,
            .minHeight = Style::controlHeight * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [this]() {
              if (m_editorSheetPopup != nullptr) {
                m_editorSheetPopup->close();
              }
            },
        })
    );
    const bool google = draft->provider == CalendarAccountProvider::Google;
    if (!draft->creating && google) {
      actions->addChild(
          ui::button({
              .text = i18n::tr("settings.calendar-accounts.save"),
              .glyph = "device-floppy",
              .variant = ButtonVariant::Secondary,
              .minHeight = Style::controlHeight * scale,
              .paddingH = Style::spaceMd * scale,
              .radius = Style::scaledRadiusMd(scale),
              .onClick = [persistAccount]() { persistAccount(true, false); },
          })
      );
    }
    actions->addChild(
        ui::button({
            .text = google ? i18n::tr("settings.calendar-accounts.save-connect")
                           : i18n::tr("settings.calendar-accounts.save"),
            .glyph = google ? "brand-google" : "device-floppy",
            .variant = ButtonVariant::Primary,
            .minHeight = Style::controlHeight * scale,
            .paddingH = Style::spaceMd * scale,
            .radius = Style::scaledRadiusMd(scale),
            .onClick = [persistAccount, google]() { persistAccount(true, google); },
        })
    );
    body.addChild(std::move(actions));
  };

  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, m_wayland->lastInputSerial()),
          .sheetTitle = title,
          .removeAction = removeAccount,
          .populateSheetBody = std::move(populateSheetBody),
          .scale = scale,
      }
  );
}

void SettingsWindow::openBarWidgetEditorSheet(
    std::string title, std::function<void(Flex&)> populate, std::function<void()> removeAction
) {
  if (m_wayland == nullptr
      || m_renderContext == nullptr
      || m_surface == nullptr
      || m_surface->xdgSurface() == nullptr
      || m_config == nullptr) {
    return;
  }

  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->close();
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_editorSheetPopup == nullptr) {
    m_editorSheetPopup = std::make_unique<settings::SettingsEditorSheetPopup>();
    m_editorSheetPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  const Config& cfg = m_config->config();
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto sctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  // In the sheet, "rebuild" means re-run the body in place; "close" tears the sheet down.
  sctx.requestRebuild = [this]() {
    if (m_editorSheetPopup != nullptr) {
      m_editorSheetPopup->rebuildBody();
    }
  };
  sctx.closeHostedEditor = [this]() { DeferredCall::callLater([this]() { closeWidgetInspectorPopup(); }); };
  // A rename changes the edited widget's id: apply it, then retitle and rebuild the sheet.
  sctx.renameWidgetInstance =
      [this](
          std::string oldName, std::string newName,
          std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
      ) {
        std::string updatedTitle = newName;
        renameWidgetInstance(std::move(oldName), std::move(newName), std::move(referenceOverrides));
        if (m_config != nullptr) {
          const std::string display = settings::widgetReferenceInfo(m_config->config(), updatedTitle).title;
          if (!display.empty()) {
            updatedTitle = display;
          }
        }
        if (m_editorSheetPopup != nullptr) {
          m_editorSheetPopup->setSheetTitle(updatedTitle);
          m_editorSheetPopup->rebuildBody();
        }
      };

  m_editorSheetFactory = std::make_unique<settings::SettingsControlFactory>(sctx);

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  const std::uint32_t grabSerial = m_pendingEditorSheetNoGrab ? 0u : m_wayland->lastInputSerial();
  m_pendingEditorSheetNoGrab = false;
  m_editorSheetPopup->open(
      settings::SettingsEditorSheetPopupRequest{
          .parent = popupParentFor(*m_surface, output, grabSerial),
          .sheetTitle = std::move(title),
          .removeAction = std::move(removeAction),
          .populateSheetBody = std::move(populate),
          .scale = scale,
      }
  );
}

void SettingsWindow::openWidgetInspectorEditor(std::vector<std::string> laneListPath, std::string widgetName) {
  DeferredCall::callLater([this, laneListPath = std::move(laneListPath), widgetName = std::move(widgetName)]() mutable {
    m_editingWidgetName = widgetName;
    m_editingCapsuleGroupId.clear();
    m_renamingWidgetName.clear();
    m_pendingDeleteWidgetName.clear();
    m_pendingDeleteWidgetSettingPath.clear();
    m_editorSheetListPath = std::move(laneListPath);
    std::string title = widgetName;
    if (m_config != nullptr) {
      const std::string display = settings::widgetReferenceInfo(m_config->config(), widgetName).title;
      if (!display.empty()) {
        title = display;
      }
    }
    openBarWidgetEditorSheet(std::move(title), [this](Flex& body) {
      if (m_editorSheetFactory == nullptr) {
        return;
      }
      auto ctx = settings::makeBarWidgetEditorContext(*m_editorSheetFactory);
      settings::buildWidgetInspectorBody(body, m_editorSheetListPath, ctx);
    });
  });
}

void SettingsWindow::openCapsuleGroupEditor(std::vector<std::string> laneListPath, std::string groupId) {
  DeferredCall::callLater([this, laneListPath = std::move(laneListPath), groupId = std::move(groupId)]() mutable {
    m_editingCapsuleGroupId = std::move(groupId);
    m_editingWidgetName.clear();
    m_renamingWidgetName.clear();
    m_pendingDeleteWidgetName.clear();
    m_pendingDeleteWidgetSettingPath.clear();
    m_editorSheetListPath = std::move(laneListPath);
    openBarWidgetEditorSheet(i18n::tr("settings.entities.widget.group.title"), [this](Flex& body) {
      if (m_editorSheetFactory == nullptr) {
        return;
      }
      auto ctx = settings::makeBarWidgetEditorContext(*m_editorSheetFactory);
      settings::buildCapsuleGroupBody(body, m_editorSheetListPath, ctx);
    });
  });
}

void SettingsWindow::openPluginSourceCreateEditor(std::optional<PluginSourceConfig> existing) {
  DeferredCall::callLater([this, existing = std::move(existing)]() {
    if (m_config == nullptr || m_pluginManager == nullptr) {
      return;
    }

    auto draft = std::make_shared<PluginSourceDraft>();
    if (existing.has_value()) {
      draft->kind = existing->kind;
      draft->name = existing->name;
      draft->location = existing->location;
      draft->autoUpdate = existing->autoUpdate;
      draft->enabled = existing->enabled;
      draft->editing = true;
    }
    const bool nameLocked = draft->editing;
    const bool fieldsLocked = draft->editing && isDefaultPluginSourceName(draft->name);
    const std::string title = draft->editing ? i18n::tr("settings.plugins.sources.edit-title")
                                             : i18n::tr("settings.plugins.sources.add-title");

    std::function<void()> removeAction;
    if (draft->editing && !isDefaultPluginSourceName(draft->name)) {
      removeAction = [this, name = draft->name]() {
        if (m_pluginManager == nullptr) {
          return;
        }
        m_pluginManager->removeSource(name);
        markPluginListDirty();
        markSettingsWriteSuccess(false);
        if (m_editorSheetPopup != nullptr) {
          m_editorSheetPopup->close();
        }
        requestSceneRebuild();
      };
    }

    openBarWidgetEditorSheet(
        title,
        [this, draft, nameLocked, fieldsLocked](Flex& body) {
          const float scale = uiScale();
          auto addField = [scale](Flex& parent, const std::string& label, std::unique_ptr<Node> control) {
            auto field = ui::column({
                .align = FlexAlign::Stretch,
                .gap = Style::spaceXs * scale,
            });
            field->addChild(
                ui::label({
                    .text = label,
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .fontWeight = FontWeight::Medium,
                })
            );
            field->addChild(std::move(control));
            parent.addChild(std::move(field));
          };

          if (!draft->error.empty()) {
            body.addChild(
                ui::label({
                    .text = draft->error,
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::Error),
                    .fontWeight = FontWeight::Medium,
                })
            );
          }

          addField(
              body, i18n::tr("settings.plugins.sources.kind-label"),
              ui::segmented({
                  .options =
                      std::vector<ui::SegmentedOption>{
                          {.label = i18n::tr("settings.plugins.sources.kind.git"), .glyph = "brand-git"},
                          {.label = i18n::tr("settings.plugins.sources.kind.path"), .glyph = "folder"},
                      },
                  .selectedIndex = pluginSourceKindIndex(draft->kind),
                  .scale = scale,
                  .enabled = !fieldsLocked,
                  .equalSegmentWidths = true,
                  .onChange = [this, draft](std::size_t index) {
                    draft->kind = index == 1 ? PluginSourceKind::Path : PluginSourceKind::Git;
                    if (draft->kind == PluginSourceKind::Path) {
                      draft->autoUpdate = false;
                    }
                    draft->error.clear();
                    if (m_editorSheetPopup != nullptr) {
                      m_editorSheetPopup->rebuildBody();
                    }
                  },
              })
          );

          Input* nameInput = nullptr;
          addField(
              body, i18n::tr("settings.plugins.sources.name-label"),
              ui::input({
                  .out = &nameInput,
                  .value = draft->name,
                  .placeholder = i18n::tr("settings.plugins.sources.name-placeholder"),
                  .invalid = draft->nameInvalid,
                  .enabled = !nameLocked,
                  .onChange = [draft](const std::string& value) {
                    draft->name = value;
                    draft->nameInvalid = false;
                    draft->error.clear();
                  },
              })
          );

          Input* locationInput = nullptr;
          addField(
              body, i18n::tr("settings.plugins.sources.location-label"),
              ui::input({
                  .out = &locationInput,
                  .value = draft->location,
                  .placeholder = draft->kind == PluginSourceKind::Git
                      ? i18n::tr("settings.plugins.sources.location-placeholder-git")
                      : i18n::tr("settings.plugins.sources.location-placeholder-path"),
                  .invalid = draft->locationInvalid,
                  .enabled = !fieldsLocked,
                  .onChange = [draft](const std::string& value) {
                    draft->location = value;
                    draft->locationInvalid = false;
                    draft->error.clear();
                  },
              })
          );

          if (draft->kind == PluginSourceKind::Git) {
            auto autoUpdate = ui::row({
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * scale,
                .fillWidth = true,
            });
            autoUpdate->addChild(
                ui::label({
                    .text = i18n::tr("settings.plugins.sources.update-on-startup"),
                    .fontSize = Style::fontSizeCaption * scale,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .fontWeight = FontWeight::Medium,
                })
            );
            autoUpdate->addChild(ui::spacer());
            autoUpdate->addChild(
                ui::toggle({
                    .checked = draft->autoUpdate,
                    .scale = scale,
                    .onChange = [draft](bool value) { draft->autoUpdate = value; },
                })
            );
            body.addChild(std::move(autoUpdate));
          }

          auto actions = ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
          });
          actions->addChild(
              ui::button({
                  .text = i18n::tr("common.actions.cancel"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .variant = ButtonVariant::Default,
                  .onClick = [this]() {
                    if (m_editorSheetPopup != nullptr) {
                      m_editorSheetPopup->close();
                    }
                  },
              })
          );
          actions->addChild(
              ui::button({
                  .text = draft->editing ? i18n::tr("settings.plugins.sources.save")
                                         : i18n::tr("settings.plugins.sources.add"),
                  .glyph = draft->editing ? "device-floppy" : "add",
                  .fontSize = Style::fontSizeCaption * scale,
                  .glyphSize = Style::fontSizeBody * scale,
                  .variant = ButtonVariant::Primary,
                  .onClick = [this, draft, nameInput, locationInput]() {
                    if (m_config == nullptr || m_pluginManager == nullptr) {
                      return;
                    }
                    draft->name = StringUtils::trim(nameInput != nullptr ? nameInput->value() : draft->name);
                    draft->location =
                        StringUtils::trim(locationInput != nullptr ? locationInput->value() : draft->location);
                    draft->nameInvalid = false;
                    draft->locationInvalid = false;
                    draft->error.clear();

                    if (!isValidPluginSourceName(draft->name)) {
                      draft->nameInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.invalid-name");
                    } else if (!draft->editing && pluginSourceNameExists(m_config->config(), draft->name)) {
                      draft->nameInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.duplicate-name");
                    } else if (draft->location.empty()) {
                      draft->locationInvalid = true;
                      draft->error = i18n::tr("settings.plugins.sources.errors.location-required");
                    }

                    if (!draft->error.empty()) {
                      if (m_editorSheetPopup != nullptr) {
                        m_editorSheetPopup->rebuildBody();
                      }
                      return;
                    }

                    m_pluginManager->addSource(
                        PluginSourceConfig{
                            .kind = draft->kind,
                            .name = draft->name,
                            .location = draft->location,
                            .autoUpdate = draft->kind == PluginSourceKind::Git && draft->autoUpdate,
                            .enabled = draft->enabled,
                        }
                    );
                    markPluginListDirty();
                    markSettingsWriteSuccess(false);
                    if (m_editorSheetPopup != nullptr) {
                      m_editorSheetPopup->close();
                    }
                    requestSceneRebuild();
                  },
              })
          );
          body.addChild(std::move(actions));
        },
        std::move(removeAction)
    );
  });
}

void SettingsWindow::openPluginSettingsEditor(std::string pluginId) {
  DeferredCall::callLater([this, pluginId = std::move(pluginId)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    const auto* manifest = scripting::PluginRegistry::instance().findManifest(pluginId);
    if (manifest == nullptr) {
      return;
    }
    const bool hasSettings =
        !manifest->settings.empty() || std::ranges::any_of(manifest->entries, [](const scripting::PluginEntry& entry) {
          return entry.kind == scripting::PluginEntryKind::Panel && !entry.settings.empty();
        });
    if (!hasSettings) {
      return;
    }

    m_editingWidgetName.clear();
    m_editingCapsuleGroupId.clear();
    m_renamingWidgetName.clear();
    m_pendingDeleteWidgetName.clear();
    m_pendingDeleteWidgetSettingPath.clear();
    m_editorSheetListPath.clear();

    const std::string title = manifest->name.empty() ? pluginId : manifest->name;
    openBarWidgetEditorSheet(title, [this, pluginId](Flex& body) {
      if (m_config == nullptr || m_editorSheetFactory == nullptr) {
        return;
      }
      const auto* currentManifest = scripting::PluginRegistry::instance().findManifest(pluginId);
      if (currentManifest == nullptr) {
        return;
      }
      settings::buildPluginSettingsEditor(
          body, m_config->config(), *m_editorSheetFactory, pluginId, *currentManifest, m_showAdvanced, uiScale()
      );
    });
  });
}

void SettingsWindow::closeWidgetInspectorPopup() {
  if (m_editorSheetPopup != nullptr) {
    m_editorSheetPopup->close();
  }
  m_editorSheetFactory.reset();
  m_editingWidgetName.clear();
  m_editingCapsuleGroupId.clear();
  requestContentRebuild();
}

void SettingsWindow::saveSupportReport() {
  if (m_config == nullptr) {
    return;
  }

  FileDialogOptions options;
  options.mode = FileDialogMode::Save;
  options.defaultFilename = "noctalia-support-report.toml";
  options.title = i18n::tr("settings.window.support-report-title");
  options.extensions = {".toml"};

  const bool opened = FileDialog::open(std::move(options), [this](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }

    auto path = *result;
    if (path.extension().empty()) {
      path += ".toml";
    }

    const std::string content = m_config->buildSupportReport();
    if (!writeTextFileAtomic(path, content)) {
      m_statusMessage = i18n::tr("settings.errors.support-report");
      m_statusIsError = true;
      requestSceneRebuild();
      return;
    }

    m_statusMessage = i18n::tr("settings.window.support-report-saved");
    m_statusIsError = false;
    requestSceneRebuild();
  });

  if (!opened) {
    m_statusMessage = i18n::tr("settings.errors.support-report");
    m_statusIsError = true;
    requestSceneRebuild();
  }
}

void SettingsWindow::saveConfigExport(settings::ConfigExportMode mode) {
  if (m_config == nullptr) {
    return;
  }

  const bool fullEffective = mode == settings::ConfigExportMode::FullEffective;

  FileDialogOptions options;
  options.mode = FileDialogMode::Save;
  options.defaultFilename = fullEffective ? "noctalia-full-config.toml" : "noctalia-config.toml";
  options.title = fullEffective ? i18n::tr("settings.export-config.full-effective-save-title")
                                : i18n::tr("settings.export-config.merged-user-save-title");
  options.extensions = {".toml"};

  const bool opened = FileDialog::open(std::move(options), [this, mode](std::optional<std::filesystem::path> result) {
    if (!result.has_value() || m_config == nullptr) {
      return;
    }

    auto path = *result;
    if (path.extension().empty()) {
      path += ".toml";
    }

    const std::string content = mode == settings::ConfigExportMode::FullEffective ? m_config->buildEffectiveConfig()
                                                                                  : m_config->buildMergedUserConfig();
    if (!writeTextFileAtomic(path, content)) {
      m_statusMessage = i18n::tr("settings.errors.export-config");
      m_statusIsError = true;
      requestSceneRebuild();
      return;
    }

    m_statusMessage = i18n::tr("settings.window.export-config-saved");
    m_statusIsError = false;
    requestSceneRebuild();
  });

  if (!opened) {
    m_statusMessage = i18n::tr("settings.errors.export-config");
    m_statusIsError = true;
    requestSceneRebuild();
  }
}
