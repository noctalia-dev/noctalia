#include "shell/bar/widgets/notification_widget.h"

#include "notification/notification_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <linux/input-event-codes.h>
#include <memory>

namespace {
  constexpr float kDotBaseSize = 6.0f;
} // namespace

NotificationWidget::NotificationWidget(NotificationManager* manager, wl_output* /*output*/, bool hideWhenNoUnread)
    : m_manager(manager), m_hideWhenNoUnread(hideWhenNoUnread) {}

void NotificationWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_RIGHT) {
      if (m_manager != nullptr) {
        const bool dndEnabled = m_manager->toggleDoNotDisturb();
        (void)dndEnabled;
      }
      requestRedraw();
      return;
    }
    if (data.button != BTN_LEFT) {
      return;
    }
    requestPanelToggle("control-center", "notifications");
  });

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "bell",
          .glyphSize = Style::barGlyphSize * m_contentScale,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  const float dotSize = kDotBaseSize * m_contentScale;
  m_dot = area->addChild(
      ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = dotSize * 0.5f,
          .width = dotSize,
          .height = dotSize,
          .visible = false,
      })
  );

  setRoot(std::move(area));
  refreshIndicatorState();
}

void NotificationWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }

  refreshIndicatorState();

  m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
  m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_glyph->setPosition(0.0f, 0.0f);
  rootNode->setSize(m_glyph->width(), m_glyph->height());

  if (m_dot != nullptr) {
    const float dotSize = kDotBaseSize * m_contentScale;
    m_dot->setPosition(m_glyph->width() - dotSize, 0.0f);
  }
}

void NotificationWidget::doUpdate(Renderer& /*renderer*/) { refreshIndicatorState(); }

void NotificationWidget::refreshIndicatorState() {
  const bool hasNotifications = (m_manager != nullptr) && m_manager->hasUnreadNotificationHistory();
  const bool dndEnabled = (m_manager != nullptr) && m_manager->doNotDisturb();

  if (Node* rootNode = root(); rootNode != nullptr) {
    const bool showWidget = !m_hideWhenNoUnread || hasNotifications;
    rootNode->setVisible(showWidget);
    rootNode->setParticipatesInLayout(showWidget);
  }

  if (hasNotifications == m_hasNotifications && dndEnabled == m_dndEnabled) {
    return;
  }
  m_hasNotifications = hasNotifications;
  m_dndEnabled = dndEnabled;
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(m_dndEnabled ? "bell-off" : "bell");
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  if (m_dot != nullptr) {
    m_dot->setVisible(m_hasNotifications && !m_dndEnabled);
  }
}
