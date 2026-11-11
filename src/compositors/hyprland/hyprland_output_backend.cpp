#include "compositors/hyprland/hyprland_output_backend.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <format>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

  constexpr Logger kLog("hyprland_output");

  std::optional<std::string> parseFocusedOutputName(std::string_view payload) {
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
          auto value = StringUtils::trim(it->get<std::string>());
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

HyprlandOutputBackend::HyprlandOutputBackend(compositors::hyprland::HyprlandRuntime& runtime)
    : compositors::hyprland::HyprlandEventHandler(runtime) {}

std::optional<std::string> HyprlandOutputBackend::focusedOutputName() const {
  // available() means the event socket is connected, so focusedmonv2 keeps the cache current.
  if (!m_runtime.available()) {
    return std::nullopt;
  }
  if (m_focusedOutputKnown) {
    return m_focusedOutput;
  }

  const auto response = m_runtime.request("j/monitors");
  if (!response.has_value()) {
    kLog.debug("failed to resolve focused output via hyprland IPC");
    return std::nullopt;
  }
  m_focusedOutput = parseFocusedOutputName(*response);
  m_focusedOutputKnown = true;
  return m_focusedOutput;
}

void HyprlandOutputBackend::handleEvent(std::string_view event, std::string_view data) {
  if (event == "focusedmonv2") {
    // focusedmonv2>>MONNAME,WORKSPACEID
    auto name = StringUtils::trim(data.substr(0, data.find(',')));
    if (name.empty()) {
      m_focusedOutputKnown = false;
      return;
    }
    m_focusedOutput = std::move(name);
    m_focusedOutputKnown = true;
    return;
  }
  // A monitor hotplug can move focus without a focusedmonv2; reseed on the next query.
  if (event.starts_with("monitoradded") || event.starts_with("monitorremoved")) {
    m_focusedOutputKnown = false;
  }
}

void HyprlandOutputBackend::notifyCleanup() {
  m_focusedOutput.reset();
  m_focusedOutputKnown = false;
}

std::unordered_map<std::string, bool> HyprlandOutputBackend::outputPowerStates() const {
  const auto response = m_runtime.requestJson("j/monitors");
  if (!response || !response->is_array()) {
    return {};
  }
  std::unordered_map<std::string, bool> states;
  for (const auto& item : *response) {
    if (!item.is_object()) {
      continue;
    }
    const auto name = item.find("name");
    const auto power = item.find("dpmsStatus");
    if (name != item.end()
        && name->is_string()
        && !name->get_ref<const std::string&>().empty()
        && power != item.end()
        && power->is_boolean()) {
      states.emplace(name->get<std::string>(), power->get<bool>());
    }
  }
  return states;
}

namespace compositors::hyprland {

  bool setOutputPower(HyprlandRuntime& runtime, bool on) {
    std::optional<std::string> response;
    if (runtime.configIsLua()) {
      response = runtime.request(std::format("dispatch hl.dsp.dpms({{ action = \"{}\"}})", on ? "enable" : "disable"));
    } else {
      response = runtime.request(std::format("dispatch dpms {}", on ? "on" : "off"));
    }
    return response && StringUtils::trim(*response) == "ok";
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
