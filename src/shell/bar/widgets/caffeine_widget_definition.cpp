#include "shell/bar/widgets/caffeine_widget_definition.h"

const noctalia::bar::WidgetDefinition<std::monostate>& caffeineWidgetDefinition() {
  static const noctalia::bar::WidgetDefinition<std::monostate> definition{
      .type = "caffeine",
  };
  return definition;
}
