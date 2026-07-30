#pragma once

#include "dbus/tray/fcitx_status.h"

#include <string>

class CompositorPlatform;
struct Config;
class OsdOverlay;
class TrayService;

class KeyboardLayoutOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void prime(const CompositorPlatform& platform);
  void onLayoutChanged(const CompositorPlatform& platform, const Config& config);
  void onTrayChanged(const TrayService& tray, const Config& config, bool enabled);

private:
  OsdOverlay* m_overlay = nullptr;
  std::string m_lastLayoutName;
  FcitxInputMethodTracker m_inputMethodTracker;
  bool m_hasLayout = false;
};
