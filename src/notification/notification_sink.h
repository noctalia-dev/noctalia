#pragma once

#include <string>

class NotificationSink {
public:
  virtual ~NotificationSink() = default;
  virtual void postError(std::string app, std::string title, std::string body) = 0;
  virtual void postInfo(std::string app, std::string title, std::string body) = 0;
};
