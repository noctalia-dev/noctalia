#include "compositors/display_backend.h"
#include "util/string_utils.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

namespace compositors::display {

  namespace {

    [[nodiscard]] std::vector<std::string> niriCmd(std::initializer_list<std::string> tail) {
      std::vector<std::string> args{"niri", "msg", "output"};
      args.insert(args.end(), tail);
      return args;
    }

    [[nodiscard]] bool jsonEnabled(const nlohmann::json& mon) {
      if (const auto it = mon.find("enabled"); it != mon.end() && it->is_boolean()) {
        return it->get<bool>();
      }
      if (const auto it = mon.find("active"); it != mon.end() && it->is_boolean()) {
        return it->get<bool>();
      }
      return !(mon.contains("logical") && mon["logical"].is_null())
          && !(mon.contains("current_mode") && mon["current_mode"].is_null());
    }

    [[nodiscard]] std::vector<OutputState> parseFetchJson(std::string_view payload, std::string& error) {
      std::vector<OutputState> outputs;
      try {
        const auto json = nlohmann::json::parse(payload);
        if (!json.is_array()) {
          error = "unexpected niri msg outputs payload";
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
          out.enabled = jsonEnabled(mon);
          out.make = mon.value("make", "");
          out.model = mon.value("model", "");
          if (const auto logical = mon.find("logical"); logical != mon.end() && logical->is_object()) {
            out.x = logical->value("x", 0);
            out.y = logical->value("y", 0);
            out.scale = logical->value("scale", 1.0);
            out.transform = transformFromName(logical->value("transform", "normal"));
          }
          out.vrr = mon.value("vrr", false);
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
              int refreshMhz = 60000;
              if (const auto r = mode.find("refresh_rate"); r != mode.end() && r->is_number()) {
                refreshMhz = static_cast<int>(std::lround(r->get<double>()));
              }
              out.modes.push_back(DisplayMode{w->get<int>(), h->get<int>(), refreshMhz, false, false});
            }
          }
          if (const auto current = mon.find("current_mode"); current != mon.end() && current->is_object()) {
            const int cw = current->value("width", 0);
            const int ch = current->value("height", 0);
            int cr = 60000;
            if (const auto r = current->find("refresh_rate"); r != current->end() && r->is_number()) {
              cr = static_cast<int>(std::lround(r->get<double>()));
            }
            for (std::size_t i = 0; i < out.modes.size(); ++i) {
              if (out.modes[i].width == cw && out.modes[i].height == ch && std::abs(out.modes[i].refreshMhz - cr) < 2) {
                out.modes[i].current = true;
                out.currentModeIndex = static_cast<int>(i);
                break;
              }
            }
          }
          if (out.modes.empty()) {
            out.modes.push_back(DisplayMode{1920, 1080, 60000, false, true});
            out.currentModeIndex = 0;
          }
          const auto current = static_cast<std::size_t>(out.currentModeIndex);
          out.modeStr = std::to_string(out.modes[current].width)
              + "x"
              + std::to_string(out.modes[current].height)
              + "@"
              + formatRateHz(out.modes[current].refreshMhz);
          outputs.push_back(std::move(out));
        }
      } catch (const std::exception& e) {
        error = e.what();
      }
      return outputs;
    }

  } // namespace

  DisplayBackendSpec niriSpec() {
    return DisplayBackendSpec{
        .kind = "niri",
        .fetchArgs = []() { return std::vector<std::string>{"niri", "msg", "--json", "outputs"}; },
        .parseFetch = [](std::string_view payload, std::string& error) { return parseFetchJson(payload, error); },
        .composeAll = [](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (!cfg.enabled) {
              cmds.push_back(DisplayCommand{niriCmd({name, "off"}), std::nullopt});
              continue;
            }
            cmds.push_back(DisplayCommand{niriCmd({name, "on"}), std::nullopt});
            cmds.push_back(DisplayCommand{niriCmd({name, "mode", cfg.modeStr}), std::nullopt});
            cmds.push_back(
                DisplayCommand{niriCmd({name, "scale", StringUtils::formatDotDecimal(cfg.scale)}), std::nullopt}
            );
            cmds.push_back(
                DisplayCommand{niriCmd({name, "transform", std::string(transformToName(cfg.transform))}), std::nullopt}
            );
            cmds.push_back(
                DisplayCommand{
                    niriCmd({name, "position", "set", "--", std::to_string(cfg.x), std::to_string(cfg.y)}), std::nullopt
                }
            );
            cmds.push_back(DisplayCommand{niriCmd({name, "vrr", cfg.vrr ? "on" : "off"}), std::nullopt});
          }
          return cmds;
        },
        .composePositions = [](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (cfg.enabled) {
              cmds.push_back(
                  DisplayCommand{
                      niriCmd({name, "position", "set", "--", std::to_string(cfg.x), std::to_string(cfg.y)}),
                      std::nullopt
                  }
              );
            }
          }
          return cmds;
        },
    };
  }

} // namespace compositors::display
