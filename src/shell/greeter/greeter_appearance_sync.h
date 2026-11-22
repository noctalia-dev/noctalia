#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

class CompositorPlatform;
class ConfigService;
class IpcService;
struct ShellGreeterSyncConfig;

namespace process {
  struct RunResult;
}

namespace greeter {

  namespace detail {

    enum class ApplyHelperProtocol : std::uint8_t {
      Unknown,
      Legacy,
      SecureSyncV1,
      CustomizationSyncV1,
    };

    ApplyHelperProtocol classifyApplyHelperProtocol(const process::RunResult& result);

  } // namespace detail

  enum class [[nodiscard]] GreeterSyncLaunch {
    Failed,
    Busy,
    LaunchedConstrained,
    LaunchedLegacy,
    StagedOnly,
  };

  struct CustomizationResult {
    bool success = false;
    bool cancelled = false;
    bool administratorOverrides = false;
    std::string error;
  };
  using CustomizationCompletion = std::function<void(CustomizationResult)>;

  using SyncCompletion = std::function<void(bool success)>;

  // True when noctalia-greeter and the privileged apply helper are installed.
  bool appearanceSyncAvailable(const ShellGreeterSyncConfig& greeterSync) noexcept;

  // Negotiates the installed helper protocol before staging. Current helpers receive
  // an appearance-only payload through pkexec and --sync; older helpers retain the
  // administrator-authenticated positional mode and its session payload. Returns Busy
  // without touching staging while another sync runs. Legacy mode may return StagedOnly
  // when no login session can host a Polkit prompt. onComplete runs on the worker thread
  // only after a successfully launched helper exits.
  GreeterSyncLaunch syncAppearanceToGreeterAsync(
      const ConfigService& config, std::string_view resolvedThemeMode, SyncCompletion onComplete = {},
      const CompositorPlatform* platform = nullptr, bool logindOnSystemBus = false
  );

  // Applies only the saved greeter profile. Requires the customization capability and
  // default state directory; never launches the legacy helper. Synchronous failures
  // populate errorOut; completion runs on the helper worker thread.
  GreeterSyncLaunch syncCustomizationToGreeterAsync(
      const ConfigService& config, std::string_view resolvedThemeMode, std::string& errorOut,
      CustomizationCompletion onComplete, const CompositorPlatform* platform = nullptr
  );

  // No-op when appearanceSyncAvailable() is false (handler is not registered).
  void registerIpc(
      IpcService& ipc, const ConfigService& config, std::function<std::string_view()> resolvedThemeMode,
      const CompositorPlatform* platform, std::function<bool()> logindOnSystemBus = {}
  );

} // namespace greeter
