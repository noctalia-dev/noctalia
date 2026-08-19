#include "shell/desktop/editor/desktop_widgets_editor_types.h"

DesktopWidgetsEditorProfile DesktopWidgetsEditorProfile::desktop() {
  return DesktopWidgetsEditorProfile{
      .logSection = "desktop",
      .layerNamespace = "noctalia-desktop-widgets-editor",
      .widgetIdPrefix = "desktop-widget-",
      .titleKey = "desktop-widgets.editor.title",
      .showLockscreenLoginPreview = false,
  };
}

DesktopWidgetsEditorProfile DesktopWidgetsEditorProfile::lockscreen() {
  return DesktopWidgetsEditorProfile{
      .logSection = "lockscreen",
      .layerNamespace = "noctalia-lockscreen-widgets-editor",
      .widgetIdPrefix = "lockscreen-widget-",
      .titleKey = "desktop-widgets.editor.title-lockscreen",
      .showLockscreenLoginPreview = true,
  };
}
