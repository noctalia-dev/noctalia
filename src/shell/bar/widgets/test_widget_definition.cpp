#include "shell/bar/widgets/test_widget_definition.h"

const noctalia::bar::WidgetDefinition<std::monostate>& testWidgetDefinition() {
  static const noctalia::bar::WidgetDefinition<std::monostate> definition{
      .type = "test",
  };
  return definition;
}
