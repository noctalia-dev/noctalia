#include "shell/bar/widgets/notification_widget_definition.h"

const noctalia::bar::WidgetDefinition<NotificationWidget::Options>& notificationWidgetDefinition() {
  using noctalia::bar::field;
  using Options = NotificationWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "notifications",
      .fields = {
          field<&Options::hideWhenNoUnread>({
              .key = "hide_when_no_unread",
          }),
      },
  };
  return definition;
}
