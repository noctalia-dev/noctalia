#pragma once

#include "shell/bar/widget.h"

struct wl_output;
class Glyph;
class Node;
class NotificationManager;

class NotificationWidget : public Widget {
public:
  NotificationWidget(NotificationManager* manager, wl_output* output, bool hideWhenNoUnread = false);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshIndicatorState();

  NotificationManager* m_manager = nullptr;
  Glyph* m_glyph = nullptr;
  Node* m_dot = nullptr;
  bool m_hideWhenNoUnread = false;
  bool m_hasNotifications = false;
  bool m_dndEnabled = false;
  bool m_openedPanelByClick = false;
};
