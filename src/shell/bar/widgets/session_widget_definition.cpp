#include "shell/bar/widgets/session_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<SessionWidget::Options>& sessionWidgetDefinition() {
  using Options = SessionWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "session",
      .fields = noctalia::bar::glyphButtonFields<Options>(),
  };
  return definition;
}
