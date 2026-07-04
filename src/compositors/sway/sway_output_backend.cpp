#include "compositors/sway/sway_output_backend.h"

#include "compositors/sway/sway_runtime.h"
#include "core/log.h"
#include "core/process/process.h"
#include "util/string_utils.h"

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

namespace compositors::sway {

  bool setOutputPower(const SwayRuntime& runtime, bool on) {
    const auto& msgCommand = runtime.outputCommand();
    if (msgCommand.empty()) {
      return false;
    }
    return process::runAsync({msgCommand, "output", "*", "dpms", on ? "on" : "off"});
  }

} // namespace compositors::sway
