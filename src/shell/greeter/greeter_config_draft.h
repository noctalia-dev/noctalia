#pragma once

#include "config/greeter_config.h"

#include <string>

class ConfigService;

// Editor-owned transaction. Reload checks happen on update and again immediately before Save.
class GreeterConfigDraft {
public:
  explicit GreeterConfigDraft(ConfigService& config);
  ShellGreeterConfig& profile() noexcept { return m_profile; }
  const ShellGreeterConfig& profile() const noexcept { return m_profile; }
  const ShellGreeterConfig& backingProfile() const noexcept { return m_backing; }
  bool dirty() const { return m_profile != m_base; }
  bool backingChanged() const { return m_backing != m_base; }
  const std::string& error() const noexcept { return m_error; }
  void refresh();
  // Explicit conflict resolution: keep the draft against the latest saved profile, or discard it.
  void keepDraft();
  void discard();
  bool save();

private:
  ConfigService& m_config;
  ShellGreeterConfig m_base;
  ShellGreeterConfig m_backing;
  ShellGreeterConfig m_profile;
  std::string m_error;
};
