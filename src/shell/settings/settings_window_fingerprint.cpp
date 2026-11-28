#include "i18n/i18n.h"
#include "security/fingerprint_manager.h"
#include "shell/settings/settings_fingerprint.h"
#include "shell/settings/settings_sheet_modal.h"
#include "shell/settings/settings_window.h"
#include "ui/controls/flex.h"

#include <algorithm>
#include <utility>

void SettingsWindow::setFingerprints(FingerprintManager* fingerprints) {
  if (m_fingerprints != nullptr && m_fingerprints != fingerprints) {
    m_fingerprints->setChangedCallback({});
  }
  m_fingerprints = fingerprints;
  if (m_fingerprints == nullptr) {
    return;
  }
  // The manager reports every state change: redraw the card and the enroll sheet. A reader
  // appearing or disappearing adds or removes the card, which needs the registry rebuilt.
  m_fingerprints->setChangedCallback([this] {
    if (!isOpen()) {
      return;
    }
    const bool showCard = !m_fingerprints->devices().empty();
    requestContentRebuild(
        /*refreshRegistry=*/showCard != m_fingerprintCardShown, /*refreshFilterRow=*/false,
        /*rebuildEditorSheet=*/true
    );
  });
}

void SettingsWindow::insertFingerprintEntry() {
  m_fingerprintCardShown = m_fingerprints != nullptr && !m_fingerprints->devices().empty();
  if (!m_fingerprintCardShown) {
    return;
  }
  settings::SettingEntry entry{
      .section = settings::SettingsSection::Security,
      .group = "fingerprint",
      .title = i18n::tr("settings.fingerprint.label"),
      .subtitle = i18n::tr("settings.fingerprint.description"),
      .path = {},
      .control =
          settings::CustomBlockSetting{
              .build =
                  [this](Flex& block) {
                    if (m_fingerprints == nullptr) {
                      return;
                    }
                    const settings::FingerprintBlockActions actions{
                        .requestRebuild = [this] { requestContentRebuild(); },
                        .openEnrollSheet = [this] { openFingerprintEnrollSheet(); },
                    };
                    settings::buildFingerprintBlock(block, *m_fingerprints, m_pendingFingerDelete, actions, uiScale());
                  },
          },
      .searchText = "fingerprint reader enroll biometric fprintd finger",
  };

  // Right after the authentication group, ahead of the privacy settings.
  auto it = std::ranges::find_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
    return e.section == settings::SettingsSection::Security && e.group == "privacy";
  });
  if (it == m_settingsRegistry.end()) {
    const auto last = std::ranges::find_last_if(m_settingsRegistry, [](const settings::SettingEntry& e) {
      return e.section == settings::SettingsSection::Security;
    });
    it = last.empty() ? m_settingsRegistry.end() : std::next(last.begin());
  }
  m_settingsRegistry.insert(it, std::move(entry));
}

void SettingsWindow::openFingerprintEnrollSheet() {
  if (m_fingerprints == nullptr || m_fingerprints->busy()) {
    return;
  }
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->close();
  }
  if (m_editorSheetModal == nullptr) {
    m_editorSheetModal = std::make_unique<settings::SettingsSheetModal>();
    m_editorSheetModal->initialize(m_modalHost, [this] { dismissOpenSelectDropdown(); });
  }
  m_fingerprints->resetEnrollment();
  const float scale = uiScale();
  const auto close = [this] {
    if (m_editorSheetModal != nullptr) {
      m_editorSheetModal->close();
    }
  };
  m_editorSheetModal->open(
      settings::SettingsSheetRequest{
          .sheetTitle = i18n::tr("settings.fingerprint.enroll-title"),
          .populateSheetBody =
              [this, close, scale](Flex& body) {
                if (m_fingerprints != nullptr) {
                  settings::buildFingerprintEnrollSheet(body, *m_fingerprints, close, scale);
                }
              },
          .scale = scale,
          .minWidth = 340.0F,
          .maxWidth = 340.0F,
          .onClosed =
              [this] {
                if (m_fingerprints != nullptr) {
                  if (m_fingerprints->mode() == FingerprintManager::Mode::Enrolling) {
                    m_fingerprints->cancel();
                  }
                  m_fingerprints->resetEnrollment();
                }
                requestContentRebuild();
              },
      }
  );
}

void SettingsWindow::stopFingerprints() {
  if (m_fingerprints != nullptr) {
    m_fingerprints->cancel();
    m_fingerprints->resetEnrollment();
  }
  m_pendingFingerDelete.clear();
}
