#pragma once

#include "config/config_types.h"

#include <string_view>

// Shared layout snapshot for desktop and lockscreen widget editors.
using DesktopWidgetsEditorSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsEditorProfile {
  std::string_view logSection;
  std::string_view layerNamespace;
  std::string_view widgetIdPrefix;
  bool showLockscreenLoginPreview = false;

  [[nodiscard]] static DesktopWidgetsEditorProfile desktop();
  [[nodiscard]] static DesktopWidgetsEditorProfile lockscreen();
};

[[nodiscard]] inline DesktopWidgetsEditorSnapshot
toDesktopWidgetsEditorSnapshot(const LockscreenWidgetsConfig& config) {
  return DesktopWidgetsEditorSnapshot{
      .enabled = config.enabled,
      .schemaVersion = config.schemaVersion,
      .grid = config.grid,
      .widgets = config.widgets,
  };
}

[[nodiscard]] inline LockscreenWidgetsConfig
fromDesktopWidgetsEditorSnapshot(const DesktopWidgetsEditorSnapshot& snapshot) {
  return LockscreenWidgetsConfig{
      .enabled = snapshot.enabled,
      .schemaVersion = snapshot.schemaVersion,
      .grid = snapshot.grid,
      .widgets = snapshot.widgets,
  };
}
