#pragma once

#include <functional>
#include <string_view>

class CompositorPlatform;
class ConfigService;
class IpcService;
struct ShellGreeterSyncConfig;

namespace greeter {

  enum class GreeterSyncLaunch {
    Failed,
    Launched,
    StagedOnly,
  };

  using SyncCompletion = std::function<void(bool success)>;

  // True when noctalia-greeter and the privileged apply helper are installed.
  [[nodiscard]] bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept;

  // Writes the current shell appearance to a staging directory, then runs the
  // configured privilege prefix (or pkexec|run0) plus noctalia-greeter-apply-appearance
  // and the staging path. When session polkit is unavailable and no privilege_command
  // override is set, returns StagedOnly after writing the staging directory.
  [[nodiscard]] GreeterSyncLaunch syncAppearanceToGreeterAsync(
      const ConfigService& config, std::string_view resolvedThemeMode, SyncCompletion onComplete = {},
      const CompositorPlatform* platform = nullptr, bool logindOnSystemBus = false
  );

  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus = {}
  );

} // namespace greeter
