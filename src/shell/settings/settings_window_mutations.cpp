#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/text/font_weight_catalog.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/settings_window.h"
#include "system/day_night_schedule.h"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

  bool settingPathNeedsSceneRebuild(const std::vector<std::string>& path) {
    if (path.size() == 2 && path[0] == "shell") {
      return path[1] == "corner_radius_scale" || path[1] == "font_family" || path[1] == "lang";
    }
    if (path.size() == 2 && path[0] == "accessibility") {
      return path[1] == "ui_scale";
    }
    return false;
  }

  bool settingPathsNeedSceneRebuild(const std::vector<std::vector<std::string>>& paths) {
    return std::ranges::any_of(paths, [](const auto& path) { return settingPathNeedsSceneRebuild(path); });
  }

  std::string settingsMutationError(const ConfigService& config, std::string fallback) {
    return config.lastMutationError().empty() ? fallback : config.lastMutationError();
  }

  bool isCustomSchedulePath(const std::vector<std::string>& path) {
    return path.size() == 2
        && path[0] == "location"
        && (path[1] == "custom_schedule" || path[1] == "sunset" || path[1] == "sunrise");
  }

} // namespace

// Custom scheduling with unusable times schedules nothing. The write itself is valid, so surface it
// as a banner rather than rejecting it — the user is likely mid-edit between the toggle and the times.
void SettingsWindow::warnOnUnusableCustomSchedule(const std::vector<std::string>& path) {
  if (m_config == nullptr || !isCustomSchedulePath(path)) {
    return;
  }
  const LocationConfig& location = m_config->config().location;
  if (location.customSchedule && !day_night_schedule::hasUsableCustomTimes(location)) {
    showTransientStatus(i18n::tr("settings.errors.custom-schedule-times"), true);
  }
}

void SettingsWindow::markSettingsWriteSuccess(bool requestRebuild) {
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->clearStatusMessage();
  }
  m_statusMessage.clear();
  m_statusIsError = false;
  m_pendingResetPageScope.clear();
  m_pendingResetSettingPaths.clear();
  if (requestRebuild) {
    requestSceneRebuild();
  }
}

void SettingsWindow::markSettingsWriteError(std::string message) {
  if (m_editorSheetPopup != nullptr && m_editorSheetPopup->isOpen()) {
    m_editorSheetPopup->setStatusMessage(std::move(message), true);
    return;
  }
  m_statusMessage = std::move(message);
  m_statusIsError = true;
  requestSceneRebuild();
}

void SettingsWindow::finishSettingsWrite(
    bool changed, bool forceSceneRebuild, bool pageResetPathsChanged, bool registryAlreadyCurrent,
    bool rebuildWhenUnchanged
) {
  const bool hadStatus = !m_statusMessage.empty();
  const bool hadPendingReset = !m_pendingResetPageScope.empty() || !m_pendingResetSettingPaths.empty();
  markSettingsWriteSuccess(false);
  if (forceSceneRebuild || hadStatus || hadPendingReset) {
    requestSceneRebuild();
  } else if (changed || rebuildWhenUnchanged) {
    requestContentRebuild(
        changed ? !registryAlreadyCurrent : true, pageResetPathsChanged || rebuildWhenUnchanged, true
    );
  }
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
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const ConfigOverrideValue patchValue = value;
    const auto previousResetPaths = currentPageResetPaths();
    if (m_config->setOverride(path, std::move(value), &changed)) {
      const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryValue(path, patchValue);
      finishSettingsWrite(changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched);
      warnOnUnusableCustomSchedule(path);
      return;
    }
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.write")));
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
    bool changed = false;
    const bool needsSceneRebuild = std::ranges::any_of(overrides, [](const auto& overrideEntry) {
      return settingPathNeedsSceneRebuild(overrideEntry.first);
    });
    const auto patchOverrides = overrides;
    const auto previousResetPaths = currentPageResetPaths();
    if (m_config->setOverrides(std::move(overrides), &changed)) {
      const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryOverrides(patchOverrides);
      finishSettingsWrite(changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched);
      return;
    }
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write")));
  });
}

void SettingsWindow::clearSettingOverride(std::vector<std::string> path) {
  DeferredCall::callLater([this, path = std::move(path)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->clearOverrides({path}, &changed)) {
      markSettingsWriteError(i18n::tr("settings.errors.write"));
      return;
    }

    const std::vector<std::vector<std::string>> paths{path};
    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::clearSettingOverrides(std::vector<std::vector<std::string>> paths) {
  DeferredCall::callLater([this, paths = std::move(paths)]() mutable {
    if (m_config == nullptr || paths.empty()) {
      return;
    }

    bool changed = false;
    const bool needsSceneRebuild = settingPathsNeedSceneRebuild(paths);
    const auto previousResetPaths = currentPageResetPaths();
    const bool success = m_config->clearOverrides(paths, &changed);
    m_pendingResetPageScope.clear();
    if (!success) {
      markSettingsWriteError(i18n::tr("settings.errors.reset-page"));
      return;
    }

    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
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
