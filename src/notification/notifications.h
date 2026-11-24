#pragma once

#include "notification/notification_sink.h"

#include <string>
#include <utility>

namespace notify {

  inline NotificationSink*& instancePtr() {
    static NotificationSink* ptr = nullptr;
    return ptr;
  }

  inline void setInstance(NotificationSink* manager) { instancePtr() = manager; }

  inline NotificationSink* instance() { return instancePtr(); }

  inline void error(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->postError(std::move(app), std::move(title), std::move(body));
    }
  }

  inline void info(std::string app, std::string title, std::string body) {
    if (auto* m = instancePtr()) {
      m->postInfo(std::move(app), std::move(title), std::move(body));
    }
  }

} // namespace notify
