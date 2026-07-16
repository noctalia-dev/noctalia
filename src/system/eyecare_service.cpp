#include "system/eyecare_service.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "idle/idle_manager.h"
#include "notification/notification_manager.h"
#include "pipewire/sound_player.h"

namespace {
  constexpr Logger kLog("eyecare");
}

void EyeCareService::initialize(
    NotificationManager* notifications, SoundPlayer* soundPlayer, IdleManager* idleManager
) {
  m_notifications = notifications;
  m_soundPlayer = soundPlayer;
  m_idleManager = idleManager;
  kLog.info("EyeCareService initialized");
}

void EyeCareService::reload(const EyeCareConfig& config) {
  m_config = config;
  if (!m_config.enabled) {
    m_activeTime = std::chrono::seconds{0};
    m_idleTime = std::chrono::seconds{0};
    m_breakTimer = std::chrono::seconds{0};
    m_breakElapsed = std::chrono::seconds{0};
    m_inBreak = false;
  }
}

void EyeCareService::onSecondTick(bool lockScreenActive) {
  if (!m_config.enabled) {
    return;
  }

  // System is idle if lockscreen is active or compositor reports session idle
  const bool isSystemIdle = lockScreenActive || (m_idleManager && m_idleManager->liveIdleSeconds() > 0);

  if (m_inBreak) {
    m_breakElapsed += std::chrono::seconds{1};
  }

  if (isSystemIdle) {
    m_idleTime += std::chrono::seconds{1};
    if (m_inBreak) {
      m_breakTimer += std::chrono::seconds{1};
      if (m_breakTimer >= std::chrono::seconds{m_config.breakDurationSeconds}) {
        triggerBreakFinished();
      }
    } else {
      // Natural break detection
      if (m_idleTime >= std::chrono::seconds{m_config.breakDurationSeconds}) {
        if (m_activeTime.count() > 0) {
          kLog.info("Natural break detected. Resetting screen time.");
          m_activeTime = std::chrono::seconds{0};
        }
      }
    }
  } else {
    // User is active on the screen
    m_idleTime = std::chrono::seconds{0};

    if (m_inBreak) {
      // User active during break. Only abort if the grace period has elapsed.
      const auto graceThreshold =
          std::min(std::chrono::seconds{10}, std::chrono::seconds{m_config.breakDurationSeconds});
      if (m_breakElapsed >= graceThreshold) {
        m_inBreak = false;
        m_breakTimer = std::chrono::seconds{0};
        m_breakElapsed = std::chrono::seconds{0};
        kLog.info("Break aborted early by user activity.");
      } else {
        kLog.debug("User active during break reminder grace period. Ignoring activity.");
      }
    } else {
      m_activeTime += std::chrono::seconds{1};
      if (m_activeTime >= std::chrono::minutes{m_config.activeDurationMinutes}) {
        triggerBreakReminder();
      }
    }
  }
}

void EyeCareService::triggerBreakReminder() {
  m_inBreak = true;
  m_breakTimer = std::chrono::seconds{0};
  m_breakElapsed = std::chrono::seconds{0};
  m_activeTime = std::chrono::seconds{0}; // Reset to prevent multiple alerts

  kLog.info("Eye care break reminder triggered.");

  if (m_notifications) {
    m_notifications->addInternal(
        i18n::tr("notifications.internal.keybind-app"), // Uses "Noctalia" as app name
        i18n::tr("notifications.internal.eyecare-break-title"), i18n::tr("notifications.internal.eyecare-break-body"),
        Urgency::Normal, 6000
    );
  }

  if (m_config.enableSound && m_soundPlayer) {
    m_soundPlayer->play("notification");
  }
}

void EyeCareService::triggerBreakFinished() {
  m_inBreak = false;
  m_breakTimer = std::chrono::seconds{0};
  m_breakElapsed = std::chrono::seconds{0};
  m_activeTime = std::chrono::seconds{0};

  kLog.info("Eye care break completed successfully.");

  if (m_notifications) {
    m_notifications->addInternal(
        i18n::tr("notifications.internal.keybind-app"), i18n::tr("notifications.internal.eyecare-break-finished-title"),
        i18n::tr("notifications.internal.eyecare-break-finished-body"), Urgency::Normal, 6000
    );
  }

  if (m_config.enableSound && m_soundPlayer) {
    m_soundPlayer->play("notification");
  }
}
