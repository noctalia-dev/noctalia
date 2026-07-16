#pragma once

#include "config/config_types.h"

#include <chrono>

class NotificationManager;
class SoundPlayer;
class IdleManager;

class EyeCareService {
public:
  EyeCareService() = default;

  void initialize(NotificationManager* notifications, SoundPlayer* soundPlayer, IdleManager* idleManager);
  void reload(const EyeCareConfig& config);
  void onSecondTick(bool lockScreenActive);

private:
  void triggerBreakReminder();
  void triggerBreakFinished();

  NotificationManager* m_notifications = nullptr;
  SoundPlayer* m_soundPlayer = nullptr;
  IdleManager* m_idleManager = nullptr;

  EyeCareConfig m_config;

  std::chrono::seconds m_activeTime{0};
  std::chrono::seconds m_idleTime{0};
  std::chrono::seconds m_breakTimer{0};
  std::chrono::seconds m_breakElapsed{0};

  bool m_inBreak = false;
};
