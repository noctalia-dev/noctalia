#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/text/font_weight_catalog.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/settings_window.h"

#include <string>
#include <utility>
#include <variant>
#include <vector>

void SettingsWindow::markSettingsWriteSuccess(bool requestRebuild) {
  m_statusMessage.clear();
  m_statusIsError = false;
  m_pendingResetPageScope.clear();
  if (requestRebuild) {
    requestSceneRebuild();
  }
}

void SettingsWindow::markSettingsWriteError(std::string message) {
  m_statusMessage = std::move(message);
  m_statusIsError = true;
  requestSceneRebuild();
}

void SettingsWindow::showTransientStatus(std::string message, bool isError) {
  m_statusMessage = std::move(message);
  m_statusIsError = isError;
  requestSceneRebuild();
}

void SettingsWindow::setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value) {
  if (path.size() == 2 && path[0] == "shell" && path[1] == "font_family") {
    text::invalidateFontWeightCatalogCache();
  }
  const bool isAvatarPath = path.size() == 2 && path[0] == "shell" && path[1] == "avatar_path";
  DeferredCall::callLater([this, path = std::move(path), value = std::move(value), isAvatarPath]() mutable {
    if (m_config == nullptr) {
      return;
    }
    if (isAvatarPath) {
      const auto* avatarPath = std::get_if<std::string>(&value);
      if (avatarPath == nullptr) {
        markSettingsWriteError(i18n::tr("settings.errors.write"));
        return;
      }
      const auto result = shell::applyAvatarPath(m_accounts, m_config, *avatarPath);
      if (result.success()) {
        markSettingsWriteSuccess();
        return;
      }
      markSettingsWriteError(i18n::tr(shell::avatarApplyErrorTranslationKey(result.error)));
      return;
    }
    if (m_config->setOverride(path, std::move(value))) {
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.write"));
  });
}

void SettingsWindow::setSettingOverrides(
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
) {
  DeferredCall::callLater([this, overrides = std::move(overrides)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    if (overrides.empty()) {
      markSettingsWriteSuccess(!m_statusMessage.empty());
      return;
    }
    if (m_config->setOverrides(std::move(overrides))) {
      markSettingsWriteSuccess(true);
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.batch-write"));
  });
}

void SettingsWindow::clearSettingOverride(std::vector<std::string> path) {
  DeferredCall::callLater([this, path = std::move(path)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    (void)m_config->clearOverride(path);
    // Rebuild even when there was nothing to clear so inherit/default controls refresh.
    markSettingsWriteSuccess();
  });
}

void SettingsWindow::clearSettingOverrides(std::vector<std::vector<std::string>> paths) {
  DeferredCall::callLater([this, paths = std::move(paths)]() mutable {
    if (m_config == nullptr || paths.empty()) {
      return;
    }

    bool changed = false;
    bool failed = false;
    for (const auto& path : paths) {
      if (m_config->clearOverride(path)) {
        changed = true;
      } else {
        failed = true;
      }
    }

    m_pendingResetPageScope.clear();
    if (failed) {
      markSettingsWriteError(i18n::tr("settings.errors.reset-page"));
      return;
    }

    markSettingsWriteSuccess(changed);
  });
}

void SettingsWindow::renameWidgetInstance(
    std::string oldName, std::string newName,
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
) {
  DeferredCall::callLater([this, oldName = std::move(oldName), newName = std::move(newName),
                           referenceOverrides = std::move(referenceOverrides)]() mutable {
    if (m_config == nullptr) {
      return;
    }

    bool changed = m_config->renameOverrideTable({"widget", oldName}, {"widget", newName});
    if (!changed) {
      markSettingsWriteError(i18n::tr("settings.errors.widget.rename"));
      return;
    }
    bool failed = false;
    for (auto& [path, value] : referenceOverrides) {
      if (m_config->setOverride(path, std::move(value))) {
        changed = true;
      } else {
        failed = true;
      }
    }
    if (failed) {
      markSettingsWriteError(i18n::tr("settings.errors.batch-write"));
      return;
    }
    markSettingsWriteSuccess(changed);
  });
}

void SettingsWindow::createBar(std::string name) {
  DeferredCall::callLater([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createBarOverride(name)) {
      m_selectedSection = "bar";
      m_selectedBarName = name;
      m_selectedMonitorOverride.clear();
      m_creatingBarName.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0f;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.create"));
  });
}

void SettingsWindow::renameBar(std::string oldName, std::string newName) {
  DeferredCall::callLater([this, oldName = std::move(oldName), newName = std::move(newName)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameBarOverride(oldName, newName)) {
      if (m_selectedBarName == oldName) {
        m_selectedBarName = newName;
      }
      m_selectedMonitorOverride.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0f;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.rename"));
  });
}

void SettingsWindow::deleteBar(std::string name) {
  DeferredCall::callLater([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteBarOverride(name)) {
      if (m_selectedBarName == name) {
        m_selectedBarName.clear();
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0f;
      }
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.delete"));
  });
}

void SettingsWindow::moveBar(std::string name, int direction) {
  DeferredCall::callLater([this, name = std::move(name), direction]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->moveBarOverride(name, direction)) {
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.move"));
  });
}

void SettingsWindow::createMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLater([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createMonitorOverride(barName, match)) {
      m_selectedSection = "bar";
      m_selectedBarName = barName;
      m_selectedMonitorOverride = match;
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0f;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.create"));
  });
}

void SettingsWindow::renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch) {
  DeferredCall::callLater([this, barName = std::move(barName), oldMatch = std::move(oldMatch),
                           newMatch = std::move(newMatch)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameMonitorOverride(barName, oldMatch, newMatch)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == oldMatch) {
        m_selectedMonitorOverride = newMatch;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0f;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.rename"));
  });
}

void SettingsWindow::deleteMonitorOverride(std::string barName, std::string match) {
  DeferredCall::callLater([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteMonitorOverride(barName, match)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == match) {
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0f;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.delete"));
  });
}
