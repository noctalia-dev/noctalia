#pragma once

#include <cstdint>
#include <string>

// Minimal configuration diagnostics interface. Shell owns the notification UI;
// shared configuration code only needs to publish and dismiss its own messages.
class ConfigNotificationSink {
public:
  virtual ~ConfigNotificationSink() = default;
  [[nodiscard]] virtual std::uint32_t postConfigError(std::string title, std::string body) = 0;
  [[nodiscard]] virtual std::uint32_t postConfigMigration(std::string title, std::string body) = 0;
  virtual void dismissConfigNotice(std::uint32_t id) = 0;
};
