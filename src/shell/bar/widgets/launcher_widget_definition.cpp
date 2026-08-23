#include "shell/bar/widgets/launcher_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<LauncherWidget::Options>& launcherWidgetDefinition() {
  using Options = LauncherWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "launcher",
      .fields = noctalia::bar::glyphButtonFields<Options>(),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
