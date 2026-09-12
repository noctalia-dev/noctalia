#include "compositors/display_backend.h"
#include "compositors/sway/sway_runtime.h"
#include "util/string_utils.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

namespace compositors::display {

  namespace {

    [[nodiscard]] int normalizeRefreshMilli(const nlohmann::json& value) {
      if (!value.is_number()) {
        return 60000;
      }
      const double raw = value.get<double>();
      if (raw <= 0.0 || !std::isfinite(raw)) {
        return 60000;
      }
      return static_cast<int>(std::lround(raw < 1000.0 ? raw * 1000.0 : raw));
    }

    [[nodiscard]] bool adaptiveSyncEnabled(const nlohmann::json& output) {
      if (const auto it = output.find("adaptive_sync_status"); it != output.end()) {
        if (it->is_boolean()) {
          return it->get<bool>();
        }
        if (it->is_string()) {
          const auto value = StringUtils::toLower(it->get<std::string>());
          return value == "enabled" || value == "on" || value == "true";
        }
      }
      return output.value("adaptive_sync", false);
    }

    [[nodiscard]] std::vector<OutputState> parseFetchJson(std::string_view payload, std::string& error) {
      std::vector<OutputState> outputs;
      try {
        const auto json = nlohmann::json::parse(payload);
        if (!json.is_array()) {
          error = "unexpected sway get_outputs payload";
          return outputs;
        }
        for (const auto& mon : json) {
          if (!mon.is_object()) {
            continue;
          }
          const auto nameIt = mon.find("name");
          if (nameIt == mon.end() || !nameIt->is_string()) {
            continue;
          }
          OutputState out;
          out.name = nameIt->get<std::string>();
          out.enabled = mon.value("active", false) || mon.value("enabled", false);
          out.make = mon.value("make", "");
          out.model = mon.value("model", "");

          const auto currentMode = mon.value("current_mode", nlohmann::json::object());
          int currentW = 0;
          int currentH = 0;
          int currentRefresh = 60000;
          if (currentMode.is_object()) {
            currentW = currentMode.value("width", 0);
            currentH = currentMode.value("height", 0);
            currentRefresh = normalizeRefreshMilli(currentMode.value("refresh", 60000));
          }
          if (const auto modesIt = mon.find("modes"); modesIt != mon.end() && modesIt->is_array()) {
            for (const auto& mode : *modesIt) {
              if (!mode.is_object()) {
                continue;
              }
              const auto w = mode.find("width");
              const auto h = mode.find("height");
              if (w == mode.end() || h == mode.end() || !w->is_number() || !h->is_number()) {
                continue;
              }
              const int mw = w->get<int>();
              const int mh = h->get<int>();
              const int refresh = normalizeRefreshMilli(mode.value("refresh", 60000));
              DisplayMode entry{mw, mh, refresh, mode.value("preferred", false), false};
              if (mode.value("current", false)
                  || (mw == currentW && mh == currentH && std::abs(refresh - currentRefresh) < 2)) {
                entry.current = true;
                out.currentModeIndex = static_cast<int>(out.modes.size());
              }
              out.modes.push_back(entry);
            }
          }
          if (out.modes.empty() && currentW > 0 && currentH > 0) {
            out.modes.push_back(DisplayMode{currentW, currentH, currentRefresh, false, true});
            out.currentModeIndex = 0;
          }

          out.scale = mon.value("scale", 1.0);
          if (out.scale <= 0.0) {
            out.scale = 1.0;
          }
          out.transform = transformFromName(mon.value("transform", "normal"));
          const auto current = static_cast<std::size_t>(out.currentModeIndex);
          const auto rect = mon.value("rect", nlohmann::json::object());
          out.x = rect.is_object() ? rect.value("x", 0) : 0;
          out.y = rect.is_object() ? rect.value("y", 0) : 0;
          out.modeStr = std::to_string(out.modes[current].width)
              + "x"
              + std::to_string(out.modes[current].height)
              + "@"
              + formatRateHz(out.modes[current].refreshMhz);
          out.vrr = adaptiveSyncEnabled(mon);
          outputs.push_back(std::move(out));
        }
      } catch (const std::exception& e) {
        error = e.what();
      }
      return outputs;
    }

  } // namespace

  DisplayBackendSpec swaySpec(compositors::sway::SwayRuntime& runtime) {
    return DisplayBackendSpec{
        .kind = "sway",
        .fetchArgs =
            [&runtime]() {
              const auto& msg = runtime.outputCommand();
              if (msg.empty()) {
                return std::vector<std::string>{};
              }
              return std::vector<std::string>{msg, "-t", "get_outputs", "-r"};
            },
        .parseFetch = [](std::string_view payload, std::string& error) { return parseFetchJson(payload, error); },
        .composeAll = [&runtime](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          const auto& msg = runtime.outputCommand();
          if (msg.empty()) {
            return {};
          }
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (!cfg.enabled) {
              cmds.push_back(DisplayCommand{{msg, "output", name, "disable"}, std::nullopt});
              continue;
            }
            cmds.push_back(DisplayCommand{{msg, "output", name, "enable"}, std::nullopt});
            cmds.push_back(DisplayCommand{{msg, "output", name, "mode", swayModeStr(cfg.modeStr)}, std::nullopt});
            cmds.push_back(
                DisplayCommand{{msg, "output", name, "scale", StringUtils::formatDotDecimal(cfg.scale)}, std::nullopt}
            );
            cmds.push_back(
                DisplayCommand{
                    {msg, "output", name, "transform", std::string(transformToName(cfg.transform))}, std::nullopt
                }
            );
            cmds.push_back(
                DisplayCommand{
                    {msg, "output", name, "position", std::to_string(cfg.x), std::to_string(cfg.y)}, std::nullopt
                }
            );
            cmds.push_back(
                DisplayCommand{{msg, "output", name, "adaptive_sync", cfg.vrr ? "on" : "off"}, std::nullopt}
            );
          }
          return cmds;
        },
        .composePositions =
            [&runtime](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          const auto& msg = runtime.outputCommand();
          if (msg.empty()) {
            return {};
          }
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (cfg.enabled) {
              cmds.push_back(
                  DisplayCommand{
                      {msg, "output", name, "position", std::to_string(cfg.x), std::to_string(cfg.y)}, std::nullopt
                  }
              );
            }
          }
          return cmds;
        },
    };
  }

} // namespace compositors::display
