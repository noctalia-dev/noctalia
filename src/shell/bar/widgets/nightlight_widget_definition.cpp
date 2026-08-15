#include "shell/bar/widgets/nightlight_widget_definition.h"

const noctalia::bar::WidgetDefinition<std::monostate>& nightlightWidgetDefinition() {
  static const noctalia::bar::WidgetDefinition<std::monostate> definition{
      .type = "nightlight",
  };
  return definition;
}
