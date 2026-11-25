#include "hooks/hook_manager.h"

#include "core/log.h"

namespace {

  constexpr Logger kLog("hooks");

} // namespace

void HookManager::setCommandRunner(CommandRunner runner) { m_runner = std::move(runner); }

void HookManager::setBlockingCommandRunner(CommandRunner runner) { m_blockingRunner = std::move(runner); }

void HookManager::reload(const HooksConfig& config) { m_config = config; }

void HookManager::fire(HookKind kind) const { (void)fireWithRunner(kind, m_runner, {}); }

bool HookManager::fireBlocking(HookKind kind) const {
  return fireWithRunner(kind, m_blockingRunner ? m_blockingRunner : m_runner, {});
}

bool HookManager::fireWithRunner(
    HookKind kind, const CommandRunner& runner, const std::vector<process::EnvOverride>& env
) const {
  if (kind == HookKind::Count || !runner) {
    return false;
  }
  const auto& cmds = m_config.commands[static_cast<std::size_t>(kind)];
  if (cmds.empty()) {
    return true;
  }
  const std::string_view name = hookKindKey(kind);
  kLog.debug("hook '{}' running {} command(s)", name, cmds.size());
  bool ok = true;
  for (const auto& cmd : cmds) {
    if (!runner(cmd, env)) {
      kLog.warn("hook '{}' command failed: {}", name, cmd);
      ok = false;
    }
  }
  return ok;
}

void HookManager::fire(HookKind kind, std::initializer_list<EnvVar> env) const {
  fireWithEnv(kind, std::span<const EnvVar>(env.begin(), env.size()));
}

void HookManager::fire(HookKind kind, const std::vector<EnvVar>& env) const { fireWithEnv(kind, env); }

void HookManager::fireWithEnv(HookKind kind, std::span<const EnvVar> env) const {
  std::vector<process::EnvOverride> overrides;
  overrides.reserve(env.size());
  for (const auto& [key, value] : env) {
    overrides.push_back({.name = key, .value = value});
  }
  (void)fireWithRunner(kind, m_runner, overrides);
}
