#include "compositors/hyprland/hyprland_output_backend.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

  constexpr Logger kLog("hyprland_output");

  [[nodiscard]] std::optional<std::string> parseFocusedOutputName(std::string_view payload) {
    if (payload.empty()) {
      return std::nullopt;
    }

    try {
      const auto json = nlohmann::json::parse(payload);
      if (!json.is_array()) {
        return std::nullopt;
      }

      for (const auto& item : json) {
        if (!item.is_object() || !item.value("focused", false)) {
          continue;
        }
        if (auto it = item.find("name"); it != item.end() && it->is_string()) {
          const auto value = StringUtils::trim(it->get<std::string>());
          if (!value.empty()) {
            return value;
          }
        }
      }
    } catch (const nlohmann::json::exception&) {
      return std::nullopt;
    }

    return std::nullopt;
  }

} // namespace

HyprlandOutputBackend::HyprlandOutputBackend(compositors::hyprland::HyprlandRuntime& runtime) : m_runtime(runtime) {}

std::optional<std::string> HyprlandOutputBackend::focusedOutputName() const {
  if (!m_runtime.available()) {
    return std::nullopt;
  }

  const auto response = m_runtime.request("j/monitors");
  if (!response.has_value()) {
    kLog.debug("failed to resolve focused output via hyprland IPC");
    return std::nullopt;
  }
  return parseFocusedOutputName(*response);
}

namespace compositors::hyprland {

  bool setOutputPower(HyprlandRuntime& runtime, bool on) {
    std::optional<std::string> response;
    if (runtime.configIsLua()) {
      response = runtime.request(std::format("dispatch hl.dsp.dpms({{ action = \"{}\"}})", on ? "enable" : "disable"));
    } else {
      response = runtime.request(std::format("dispatch dpms {}", on ? "on" : "off"));
    }
    return response != std::nullopt;
  }

  bool focusOutput(HyprlandRuntime& runtime, std::string_view connectorName) {
    if (!runtime.available() || connectorName.empty()) {
      return false;
    }
    std::optional<std::string> response;
    if (runtime.configIsLua()) {
      response = runtime.request(std::format("dispatch hl.dsp.focus({{ monitor = \"{}\" }})", connectorName));
    } else {
      response = runtime.request(std::format("dispatch focusmonitor {}", connectorName));
    }
    if (!response.has_value()) {
      kLog.debug("failed to focus monitor {}", connectorName);
      return false;
    }
    return true;
  }

  bool moveWindowToOutput(HyprlandRuntime& runtime, std::string_view windowSelector, std::string_view connectorName) {
    if (!runtime.available() || windowSelector.empty() || connectorName.empty()) {
      return false;
    }
    std::optional<std::string> response;
    if (runtime.configIsLua()) {
      response = runtime.request(
          std::format(
              R"(dispatch hl.dsp.window.move({{ monitor = "{}", window = "{}", follow = false }}))", connectorName,
              windowSelector
          )
      );
    } else {
      response =
          runtime.request(std::format("dispatch movetoworkspacesilent mon:{},{}", connectorName, windowSelector));
    }
    if (!response.has_value()) {
      kLog.debug("failed to move window {} to monitor {}", windowSelector, connectorName);
      return false;
    }
    return true;
  }

} // namespace compositors::hyprland
