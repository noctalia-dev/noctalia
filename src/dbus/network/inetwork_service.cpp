#include "dbus/network/inetwork_service.h"

#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <optional>
#include <string>
#include <string_view>

namespace {

  std::optional<std::string> rejectArgs(std::string_view command, const std::string& args) {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return "error: " + std::string(command) + " takes no arguments\n";
  }

} // namespace

void INetworkService::registerIpc(IpcService& ipc, WirelessFeedbackCallback wirelessFeedback) {
  auto setWifi = [this, wirelessFeedback](bool enabled) -> std::string {
    if (!hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    if (state().wirelessEnabled == enabled) {
      return "ok\n";
    }
    setWirelessEnabled(enabled);
    if (wirelessFeedback) {
      wirelessFeedback(enabled);
    }
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::wifiEnable, [setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-enable", args); err.has_value()) {
      return *err;
    }
    return setWifi(true);
  });

  ipc.bind(noctalia::cli::msg::wifiDisable, [setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-disable", args); err.has_value()) {
      return *err;
    }
    return setWifi(false);
  });

  ipc.bind(noctalia::cli::msg::wifiToggle, [this, setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("wifi-toggle", args); err.has_value()) {
      return *err;
    }
    if (!hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    return setWifi(!state().wirelessEnabled);
  });

  ipc.bind(
      noctalia::cli::msg::wifiStatus,
      [this](const std::string& args) -> std::string {
        if (auto err = rejectArgs("wifi-status", args); err.has_value()) {
          return *err;
        }
        if (!hasStateSnapshot()) {
          return "error: network state unavailable\n";
        }
        return state().wirelessEnabled ? "on\n" : "off\n";
      },
      IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
  );

  ipc.bind(noctalia::cli::msg::networkToggle, [this, setWifi](const std::string& args) -> std::string {
    if (auto err = rejectArgs("network-toggle", args); err.has_value()) {
      return *err;
    }
    if (!hasStateSnapshot()) {
      return "error: network state unavailable\n";
    }
    const NetworkState& s = state();
    // Drop whatever is up, otherwise bring back whichever transport can come up.
    if (s.kind == NetworkConnectivity::Wireless && (s.connected || s.resolving)) {
      return setWifi(false);
    }
    if (s.kind == NetworkConnectivity::Wired && (s.connected || s.resolving)) {
      disconnect();
      return "ok\n";
    }
    if (!s.wirelessEnabled) {
      return setWifi(true);
    }
    if (canActivateWiredConnection()) {
      if (!activateWiredConnection()) {
        return "error: failed to activate the wired connection\n";
      }
      return "ok\n";
    }
    return "error: nothing to toggle (Wi-Fi is on and no wired connection is available)\n";
  });
}
