#include "compositors/sway/sway_output_backend.h"

#include "compositors/sway/sway_runtime.h"
#include "core/log.h"
#include "core/process/process.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string_view>

namespace {

  constexpr Logger kLog("sway_output");

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

SwayOutputBackend::SwayOutputBackend(compositors::sway::SwayRuntime& runtime) : m_runtime(runtime) {}

std::optional<std::string> SwayOutputBackend::focusedOutputName() const {
  const auto& msgCommand = m_runtime.outputCommand();
  if (msgCommand.empty()) {
    return std::nullopt;
  }

  const auto result = process::runSync({msgCommand, "-t", "get_outputs", "-r"});
  if (!result) {
    kLog.debug("failed to resolve focused output via {}", msgCommand);
    return std::nullopt;
  }
  return parseFocusedOutputName(result.out);
}

std::unordered_map<std::string, bool> SwayOutputBackend::outputPowerStates() const {
  const auto& msgCommand = m_runtime.outputCommand();
  if (msgCommand.empty()) {
    return {};
  }
  const auto result =
      process::runSyncWithTimeout({msgCommand, "-t", "get_outputs", "-r"}, std::chrono::milliseconds(500));
  if (!result) {
    kLog.debug("failed to query output power via {}", msgCommand);
    return {};
  }
  std::unordered_map<std::string, bool> states;
  try {
    const auto json = nlohmann::json::parse(result.out);
    if (!json.is_array()) {
      return {};
    }
    for (const auto& item : json) {
      if (!item.is_object()) {
        continue;
      }
      const auto name = item.find("name");
      const auto power = item.find("power");
      if (name != item.end()
          && name->is_string()
          && !name->get_ref<const std::string&>().empty()
          && power != item.end()
          && power->is_boolean()) {
        states.emplace(name->get<std::string>(), power->get<bool>());
      }
    }
  } catch (const nlohmann::json::exception&) {
    kLog.debug("invalid output power response from {}", msgCommand);
    return {};
  }
  return states;
}

namespace compositors::sway {

  bool setOutputPower(const SwayRuntime& runtime, bool on) {
    const auto& msgCommand = runtime.outputCommand();
    if (msgCommand.empty()) {
      return false;
    }
    const auto result = process::runSyncWithTimeout(
        {msgCommand, "-r", "output", "*", "power", on ? "on" : "off"}, std::chrono::milliseconds(500)
    );
    if (!result) {
      return false;
    }
    try {
      const auto json = nlohmann::json::parse(result.out);
      if (!json.is_array() || json.empty()) {
        return false;
      }
      return std::ranges::all_of(json, [&](const nlohmann::json& item) {
        if (!item.is_object()) {
          return false;
        }
        const auto success = item.find("success");
        return success != item.end() && success->is_boolean() && success->get<bool>();
      });
    } catch (const nlohmann::json::exception&) {
      return false;
    }
  }

} // namespace compositors::sway
