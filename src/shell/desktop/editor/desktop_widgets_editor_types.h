#pragma once

#include "config/config_types.h"

#include <string_view>

// Shared layout snapshot for desktop and lockscreen widget editors.
using DesktopWidgetsEditorSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsEditorProfile {
  std::string_view logSection;
  std::string_view layerNamespace;
  std::string_view widgetIdPrefix;
  std::string_view titleKey;
  bool showLockscreenLoginPreview = false;

  static DesktopWidgetsEditorProfile desktop();
  static DesktopWidgetsEditorProfile lockscreen();
};

inline DesktopWidgetsEditorSnapshot toDesktopWidgetsEditorSnapshot(const LockscreenWidgetsConfig& config) {
  return DesktopWidgetsEditorSnapshot{
      .enabled = config.enabled,
      .schemaVersion = config.schemaVersion,
      .grid = config.grid,
      .widgets = config.widgets,
  };
}

inline LockscreenWidgetsConfig fromDesktopWidgetsEditorSnapshot(const DesktopWidgetsEditorSnapshot& snapshot) {
  return LockscreenWidgetsConfig{
      .enabled = snapshot.enabled,
      .schemaVersion = snapshot.schemaVersion,
      .grid = snapshot.grid,
      .widgets = snapshot.widgets,
  };
}
