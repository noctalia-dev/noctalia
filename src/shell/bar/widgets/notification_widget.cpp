#include "shell/bar/widgets/notification_widget.h"

#include "notification/notification_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>

namespace {
  constexpr float kDotBaseSize = 6.0F;
} // namespace

NotificationWidget::NotificationWidget(NotificationManager* manager, wl_output* /*output*/, Options options)
    : m_manager(manager), m_hideWhenNoUnread(options.hideWhenNoUnread) {}

void NotificationWidget::create() {
  auto area = ui::inputArea({});

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bell",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  const float dotSize = kDotBaseSize * m_contentScale;
  m_dot = area->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = dotSize * 0.5F,
          .width = dotSize,
          .height = dotSize,
          .visible = false,
      })
  );

  setRoot(std::move(area));
  refreshIndicatorState();
}

// hide_when_no_unread would pull the widget out from under the pointer the moment the panel it
// just opened marks everything read. The latch clears itself once that panel closes.
void NotificationWidget::onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) {
  (void)gesture;
  (void)action;
  m_openedPanelByClick = true;
}

void NotificationWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  refreshIndicatorState();
  if (!rootNode->visible()) {
    rootNode->setSize(0.0F, 0.0F);
    return;
  }

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_glyph->setPosition(0.0F, 0.0F);
  rootNode->setSize(m_glyph->width(), m_glyph->height());

  if (m_dot != nullptr) {
    const float dotSize = kDotBaseSize * m_contentScale;
    m_dot->setPosition(m_glyph->width() - dotSize, 0.0F);
  }
}

void NotificationWidget::doUpdate(Renderer& /*renderer*/) { refreshIndicatorState(); }

void NotificationWidget::refreshIndicatorState() {
  const bool hasNotifications = (m_manager != nullptr) && m_manager->hasUnreadNotificationHistory();
  const bool dndEnabled = (m_manager != nullptr) && m_manager->doNotDisturb();

  if (Node* rootNode = root(); rootNode != nullptr) {
    if (m_openedPanelByClick) {
      m_openedPanelByClick = PanelManager::instance().isOpenPanel("control-center");
    }
    const bool showWidget = m_openedPanelByClick || !m_hideWhenNoUnread || hasNotifications;
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
    if (!showWidget) {
      rootNode->setSize(0.0F, 0.0F);
    }
  }

  if (hasNotifications == m_hasNotifications && dndEnabled == m_dndEnabled) {
    return;
  }
  m_hasNotifications = hasNotifications;
  m_dndEnabled = dndEnabled;
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  if (m_dot != nullptr) {
    m_dot->setVisible(m_hasNotifications && !m_dndEnabled);
  }
}
