#pragma once

#include "shell/bar/widget.h"

struct wl_output;
class Glyph;
class Node;
class NotificationManager;
class JobProgressManager;

class NotificationWidget : public Widget {
public:
  struct Options {
    bool hideWhenNoUnread = false;
  };

  NotificationWidget(NotificationManager* manager, JobProgressManager* jobs, wl_output* output, Options options);

  void create() override;

private:
  void onGestureDispatch(nythera::bar::Gesture gesture, const nythera::bar::WidgetAction& action) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void refreshIndicatorState();

  NotificationManager* m_manager = nullptr;
  JobProgressManager* m_jobs = nullptr;
  Glyph* m_glyph = nullptr;
  Node* m_dot = nullptr;
  bool m_hideWhenNoUnread = false;
  bool m_hasNotifications = false;
  bool m_hasJobs = false;
  bool m_dndEnabled = false;
  bool m_openedPanelByClick = false;
};
