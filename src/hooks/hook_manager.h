#pragma once

#include "config/config_types.h"

#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

class HookManager {
public:
  using CommandRunner = std::function<bool(const std::string& command)>;
  using EnvVar = std::pair<const char*, std::string>;

  void setCommandRunner(CommandRunner runner);
  void setBlockingCommandRunner(CommandRunner runner);
  void reload(const HooksConfig& config);
  void fire(HookKind kind) const;
  bool fireBlocking(HookKind kind) const;
  void fire(HookKind kind, std::initializer_list<EnvVar> env) const;
  void fire(HookKind kind, const std::vector<EnvVar>& env) const;

  const HooksConfig& config() const noexcept { return m_config; }

private:
  bool fireWithRunner(HookKind kind, const CommandRunner& runner) const;
  void fireWithEnv(HookKind kind, std::span<const EnvVar> env) const;

  HooksConfig m_config;
  CommandRunner m_runner;
  CommandRunner m_blockingRunner;
};
