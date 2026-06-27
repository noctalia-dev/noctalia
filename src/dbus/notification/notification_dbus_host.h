#pragma once

class NotificationDBusHost {
public:
  virtual ~NotificationDBusHost() = default;

  virtual void processExpired() = 0;
  [[nodiscard]] virtual bool isHealthy() const = 0;
};
