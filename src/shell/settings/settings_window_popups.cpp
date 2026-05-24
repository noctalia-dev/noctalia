#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/render_context.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/settings_window.h"
#include "ui/controls/button.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/flex.h"
#include "ui/dialogs/file_dialog.h"
#include "util/string_utils.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr std::int32_t kActionSupportReport = 1;
  constexpr std::int32_t kActionExportConfig = 2;

  std::string sessionActionTitle(const SessionPanelActionConfig& row) {
    if (row.label.has_value() && !StringUtils::trim(*row.label).empty()) {
      return *row.label;
    }
    if (row.action == "lock") {
      return i18n::tr("settings.session-actions.kind.lock");
    }
    if (row.action == "logout") {
      return i18n::tr("settings.session-actions.kind.logout");
    }
    if (row.action == "suspend") {
      return i18n::tr("settings.session-actions.kind.suspend");
    }
    if (row.action == "reboot") {
      return i18n::tr("settings.session-actions.kind.reboot");
    }
    if (row.action == "shutdown") {
      return i18n::tr("settings.session-actions.kind.shutdown");
    }
    if (row.action == "command") {
      return i18n::tr("settings.session-actions.kind.command");
    }
    return row.action;
  }

  std::string idleBehaviorTitle(const IdleBehaviorConfig& row) {
    IdleBehaviorConfig norm = row;
    inferIdleBehaviorActionFromLegacyFields(norm);
    if (norm.action == "lock") {
      return i18n::tr("settings.idle.behavior.presets.lock");
    }
    if (norm.action == "screen_off") {
      return i18n::tr("settings.idle.behavior.presets.monitor-off");
    }
    if (norm.action == "suspend") {
      return i18n::tr("settings.idle.behavior.presets.suspend");
    }
    if (!StringUtils::trim(row.name).empty()) {
      return row.name;
    }
    return i18n::tr("settings.idle.behavior.unnamed");
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
      for (int suffix = 2; std::find(used.begin(), used.end(), candidate) != used.end(); ++suffix) {
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
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr || m_actionsMenuButton == nullptr ||
      m_surface->xdgSurface() == nullptr) {
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
  m_actionsMenuPopup->openAsChild(
      std::move(entries), 220.0f * scale, 8, static_cast<std::int32_t>(anchorAbsX),
      static_cast<std::int32_t>(anchorAbsY), static_cast<std::int32_t>(m_actionsMenuButton->width()),
      static_cast<std::int32_t>(m_actionsMenuButton->height()), m_surface->xdgSurface(), output
  );
}

void SettingsWindow::openConfigExportDialog() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr) {
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
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), uiScale(), [this](settings::ConfigExportMode mode) { saveConfigExport(mode); }
  );
}

void SettingsWindow::openBarWidgetAddPopup(const std::vector<std::string>& lanePath) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr) {
    return;
  }

  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
    m_sessionActionsEditorPopup->close();
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
        overrides.push_back({selectedLanePath, laneItems});
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
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), lanePath, m_config->config(), uiScale()
  );
}

void SettingsWindow::openSearchPickerPopup(
    const std::string& title, const std::vector<settings::SelectOption>& options, const std::string& selectedValue,
    const std::string& placeholder, const std::string& emptyText, const std::vector<std::string>& settingPath
) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr || options.empty()) {
    return;
  }

  if (m_searchPickerPopup == nullptr) {
    m_searchPickerPopup = std::make_unique<settings::SearchPickerPopup>();
    m_searchPickerPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }

  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
    m_sessionActionsEditorPopup->close();
  }

  m_searchPickerPopup->setOnSelect([this, settingPath, selectedValue](const std::string& value) {
    if (value != selectedValue) {
      setSettingOverride(settingPath, value);
    }
  });

  std::vector<SearchPickerOption> pickerOptions;
  pickerOptions.reserve(options.size());
  for (const auto& opt : options) {
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

  m_searchPickerPopup->open(
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), title, pickerOptions, selectedValue, placeholder, emptyText, uiScale()
  );
}

void SettingsWindow::openSessionActionEntryEditor(std::size_t index) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr) {
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

  if (m_sessionActionsEditorPopup == nullptr) {
    m_sessionActionsEditorPopup = std::make_unique<settings::SessionActionsEditorPopup>();
    m_sessionActionsEditorPopup->initialize(*m_wayland, *m_config, *m_renderContext);
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
    setSettingOverride({"shell", "session", "actions"}, next);
    requestContentRebuild();
    if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
      m_sessionActionsEditorPopup->requestLayout();
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
    if (m_sessionActionsEditorPopup != nullptr) {
      m_sessionActionsEditorPopup->close();
    }
    requestContentRebuild();
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.closeHostedEditor = [this]() {
    if (m_sessionActionsEditorPopup != nullptr) {
      m_sessionActionsEditorPopup->close();
    }
  };

  const std::string sheetTitle = sessionActionTitle(*rowState);

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_sessionActionsEditorPopup->open(
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), scale, sheetTitle, removeRow, [ctx, rowState, persist](Flex& body) mutable {
        settings::buildSessionActionEntryDetailContent(body, ctx, *rowState, persist);
      }
  );
}

void SettingsWindow::openIdleBehaviorEntryEditor(std::size_t index) {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr) {
    return;
  }

  // Closing the previous hosted editor can commit focused fields via focus-loss callbacks.
  // Do it before reading cfg/rowState so the new editor is built from the latest config.
  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
    m_sessionActionsEditorPopup->close();
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

  if (m_sessionActionsEditorPopup == nullptr) {
    m_sessionActionsEditorPopup = std::make_unique<settings::SessionActionsEditorPopup>();
    m_sessionActionsEditorPopup->initialize(*m_wayland, *m_config, *m_renderContext);
  }
  const float scale = uiScale();
  const BarConfig* selectedBar = settings::findBar(cfg, m_selectedBarName);
  const BarMonitorOverride* selectedMonitorOverride = nullptr;
  if (selectedBar != nullptr && !m_selectedMonitorOverride.empty()) {
    selectedMonitorOverride = settings::findMonitorOverride(*selectedBar, m_selectedMonitorOverride);
  }

  auto rowState = std::make_shared<IdleBehaviorConfig>(cfg.idle.behaviors[index]);
  auto rowKey = std::make_shared<std::string>(rowState->name);
  inferIdleBehaviorActionFromLegacyFields(*rowState);

  const auto persist = [this, rowState, rowKey, index]() {
    if (m_config == nullptr) {
      return;
    }
    inferIdleBehaviorActionFromLegacyFields(*rowState);
    auto next = m_config->config().idle.behaviors;
    auto target = std::find_if(next.begin(), next.end(), [rowKey](const IdleBehaviorConfig& behavior) {
      return behavior.name == *rowKey;
    });
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
    if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
      m_sessionActionsEditorPopup->requestLayout();
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
    if (m_sessionActionsEditorPopup != nullptr) {
      m_sessionActionsEditorPopup->close();
    }
    requestContentRebuild();
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.closeHostedEditor = [this]() {
    if (m_sessionActionsEditorPopup != nullptr) {
      m_sessionActionsEditorPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_sessionActionsEditorPopup->open(
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), scale, idleBehaviorTitle(*rowState), removeRow,
      [ctx, rowState, persist](Flex& body) mutable {
        settings::buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persist);
      }
  );
}

void SettingsWindow::openIdleBehaviorCreateEditor() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_surface == nullptr ||
      m_surface->xdgSurface() == nullptr || m_config == nullptr) {
    return;
  }

  if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
    m_sessionActionsEditorPopup->close();
  }
  if (m_widgetAddPopup != nullptr && m_widgetAddPopup->isOpen()) {
    m_widgetAddPopup->close();
  }
  if (m_searchPickerPopup != nullptr && m_searchPickerPopup->isOpen()) {
    m_searchPickerPopup->close();
  }

  if (m_sessionActionsEditorPopup == nullptr) {
    m_sessionActionsEditorPopup = std::make_unique<settings::SessionActionsEditorPopup>();
    m_sessionActionsEditorPopup->initialize(*m_wayland, *m_config, *m_renderContext);
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
    if (m_sessionActionsEditorPopup != nullptr && m_sessionActionsEditorPopup->isOpen()) {
      m_sessionActionsEditorPopup->requestLayout();
    }
  };

  auto ctx = makeContentContext(cfg, selectedBar, selectedMonitorOverride);
  ctx.openSessionActionEntryEditor = {};
  ctx.openIdleBehaviorEntryEditor = {};
  ctx.afterIdleBehaviorApply = [this, rowState]() {
    if (m_config == nullptr) {
      return;
    }
    inferIdleBehaviorActionFromLegacyFields(*rowState);
    auto next = m_config->config().idle.behaviors;
    next.push_back(*rowState);
    normalizeIdleBehaviorNames(next);
    setSettingOverride({"idle", "behavior"}, next);
    requestContentRebuild();
  };
  ctx.closeHostedEditor = [this]() {
    if (m_sessionActionsEditorPopup != nullptr) {
      m_sessionActionsEditorPopup->close();
    }
  };

  wl_output* output = m_wayland->lastPointerOutput();
  if (output == nullptr) {
    output = m_output;
  }

  m_sessionActionsEditorPopup->open(
      m_surface->xdgSurface(), output, m_wayland->lastInputSerial(), m_surface->wlSurface(), m_surface->width(),
      m_surface->height(), scale, idleBehaviorTitle(*rowState), nullptr,
      [ctx, rowState, persistDraft](Flex& body) mutable {
        settings::buildIdleBehaviorEntryDetailContent(body, ctx, *rowState, persistDraft);
      }
  );
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

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
      m_statusMessage = i18n::tr("settings.errors.support-report");
      m_statusIsError = true;
      requestSceneRebuild();
      return;
    }

    out << m_config->buildSupportReport();
    if (!out.good()) {
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

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
      m_statusMessage = i18n::tr("settings.errors.export-config");
      m_statusIsError = true;
      requestSceneRebuild();
      return;
    }

    const std::string content = mode == settings::ConfigExportMode::FullEffective ? m_config->buildEffectiveConfig()
                                                                                  : m_config->buildMergedUserConfig();
    out << content;
    if (!out.good()) {
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
