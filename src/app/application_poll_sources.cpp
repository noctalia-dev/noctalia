#include "application.h"
#include "application_internal.h"
#include "dbus/idle/screensaver_poll_source.h"
#include "dbus/idle/screensaver_service.h"
#include "dbus/polkit/polkit_poll_source.h"
#include "dbus/session_bus_poll_source.h"
#include "dbus/system_bus_poll_source.h"
#include "pipewire/pipewire_poll_source.h"
#include "pipewire/pipewire_spectrum_poll_source.h"
#include "pipewire/wireplumber_mixer.h"
#include "system/brightness_poll_source.h"

std::vector<PollSource*> Application::currentPollSources() {
  std::vector<PollSource*> sources;
  if (m_busPollSource != nullptr) {
    sources.push_back(m_busPollSource.get());
  }
  if (m_screenSaverService != nullptr
      && m_screenSaverService->hasScreenSaverBus()
      && m_screenSaverPollSource != nullptr) {
    sources.push_back(m_screenSaverPollSource.get());
  }
  if (m_systemBusPollSource != nullptr) {
    sources.push_back(m_systemBusPollSource.get());
  }
  sources.push_back(&m_notificationPollSource);
  sources.push_back(&m_secretStore);
  sources.push_back(&m_deferredCallPollSource);
  sources.push_back(&m_timePollSource);
  sources.push_back(&m_configPollSource);
  sources.push_back(&m_desktopEntryPollSource);
  sources.push_back(&m_iconThemePollSource);
  sources.push_back(&m_clipboardPollSource);
  sources.push_back(&m_timerPollSource);
  sources.push_back(&m_keyRepeatPollSource);
  sources.push_back(&m_workspacePollSource);
  sources.push_back(&m_keyboardLayoutPollSource);
  if constexpr (kLockKeysEnabled) {
    if (lockKeysConsumersEnabled(m_configService.config())) {
      sources.push_back(&m_lockKeysPollSource);
    }
  }
  if (m_pipewirePollSource != nullptr) {
    sources.push_back(m_pipewirePollSource.get());
  }
  if (m_pipewireSpectrumPollSource != nullptr) {
    sources.push_back(m_pipewireSpectrumPollSource.get());
  }
  if (m_wirePlumberMixer != nullptr) {
    sources.push_back(m_wirePlumberMixer.get());
  }
  if (m_polkitPollSource != nullptr) {
    sources.push_back(m_polkitPollSource.get());
  }
  if (m_brightnessPollSource != nullptr) {
    sources.push_back(m_brightnessPollSource.get());
  }
  sources.push_back(&m_fileWatchPollSource);
  sources.push_back(&m_ipcPollSource);
  sources.push_back(&m_dmenuIpc);
  sources.push_back(&m_httpClientPollSource);
  sources.push_back(&m_locationPollSource);
  sources.push_back(&m_weatherPollSource);
  sources.push_back(&m_calendarPollSource);
  sources.push_back(&m_thumbnailService);
  sources.push_back(&m_wallpaperScanner);
  sources.push_back(&m_asyncTextureCache);
  return sources;
}

std::vector<PollSource*> Application::buildPollSources() {
  if (m_bus != nullptr) {
    if (m_busPollSource == nullptr) {
      m_busPollSource = std::make_unique<SessionBusPollSource>(*m_bus);
    }
  } else {
    m_busPollSource.reset();
  }
  if (m_screenSaverService != nullptr && m_screenSaverService->hasScreenSaverBus()) {
    if (m_screenSaverPollSource == nullptr) {
      m_screenSaverPollSource = std::make_unique<ScreenSaverPollSource>(*m_screenSaverService);
    }
  } else {
    m_screenSaverPollSource.reset();
  }
  if (m_systemBus != nullptr) {
    if (m_systemBusPollSource == nullptr) {
      m_systemBusPollSource = std::make_unique<SystemBusPollSource>(*m_systemBus);
    }
  } else {
    m_systemBusPollSource.reset();
  }
  if (m_pipewireService != nullptr) {
    if (m_pipewirePollSource == nullptr) {
      m_pipewirePollSource = std::make_unique<PipeWirePollSource>(*m_pipewireService);
    }
  } else {
    m_pipewirePollSource.reset();
  }
  if (m_pipewireSpectrum != nullptr) {
    if (m_pipewireSpectrumPollSource == nullptr) {
      m_pipewireSpectrumPollSource = std::make_unique<PipeWireSpectrumPollSource>(*m_pipewireSpectrum);
    }
  } else {
    m_pipewireSpectrumPollSource.reset();
  }
  if (m_brightnessService != nullptr) {
    if (m_brightnessPollSource == nullptr) {
      m_brightnessPollSource = std::make_unique<BrightnessPollSource>(*m_brightnessService);
    }
  } else {
    m_brightnessPollSource.reset();
  }
  return currentPollSources();
}
