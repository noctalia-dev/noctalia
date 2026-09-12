#include "compositors/display_backend.h"
#include "util/string_utils.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <string>

namespace compositors::display {

  namespace {

    [[nodiscard]] std::string monitorKeywordArg(const std::string& outputName, const OutputState& cfg) {
      const std::string mode = cfg.modeStr.empty() ? "preferred" : cfg.modeStr;
      const auto scale = StringUtils::formatDotDecimal(cfg.scale);
      return outputName
          + ","
          + mode
          + ","
          + std::to_string(cfg.x)
          + "x"
          + std::to_string(cfg.y)
          + ","
          + scale
          + ",transform,"
          + std::to_string(transformToCode(cfg.transform));
    }

    [[nodiscard]] std::vector<std::string> monitorKeyword(const std::string& outputName, const OutputState& cfg) {
      return {"hyprctl", "keyword", "monitor", monitorKeywordArg(outputName, cfg)};
    }

    [[nodiscard]] DisplayCommand hdrCommand(const std::string& outputName, const OutputState& cfg) {
      return DisplayCommand{
          {"hyprctl", "keyword", "monitor", monitorKeywordArg(outputName, cfg) + ",bitdepth," + (cfg.hdr ? "10" : "8")},
          std::nullopt
      };
    }

    [[nodiscard]] std::vector<OutputState> parseFetchJson(std::string_view payload, std::string& error) {
      std::vector<OutputState> outputs;
      try {
        const auto json = nlohmann::json::parse(payload);
        if (!json.is_array()) {
          error = "unexpected hyprctl monitors payload";
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
          out.enabled = !mon.value("disabled", false);
          out.make = mon.value("make", "");
          out.model = mon.value("model", "");
          out.x = mon.value("x", 0);
          out.y = mon.value("y", 0);
          out.scale = mon.value("scale", 1.0);
          out.transform = transformFromCode(mon.value("transform", 0));
          out.vrr = mon.value("vrr", false);
          out.hdr = mon.value("hdr", false);
          const int width = mon.value("width", 0);
          const int height = mon.value("height", 0);
          const double refreshRate = mon.value("refreshRate", 0.0);
          if (const auto modesIt = mon.find("availableModes"); modesIt != mon.end() && modesIt->is_array()) {
            for (const auto& modeStr : *modesIt) {
              if (!modeStr.is_string()) {
                continue;
              }
              const std::string raw = modeStr.get<std::string>();
              const auto at = raw.find('@');
              if (at == std::string::npos) {
                continue;
              }
              const auto xPos = raw.find('x');
              if (xPos == std::string::npos || xPos > at) {
                continue;
              }
              const int mw = std::stoi(raw.substr(0, xPos));
              const int mh = std::stoi(raw.substr(xPos + 1, at - xPos - 1));
              std::string rateStr = raw.substr(at + 1);
              if (rateStr.ends_with("Hz")) {
                rateStr = rateStr.substr(0, rateStr.size() - 2);
              }
              const int rateMhz = static_cast<int>(std::lround(std::stof(rateStr) * 1000.0));
              DisplayMode mode{mw, mh, rateMhz, false, false};
              if (mw == width && mh == height && std::abs(rateMhz / 1000.0 - refreshRate) < 1.0) {
                mode.current = true;
                out.currentModeIndex = static_cast<int>(out.modes.size());
              }
              out.modes.push_back(mode);
            }
          }
          if (out.modes.empty()) {
            out.modes.push_back(
                DisplayMode{width, height, static_cast<int>(std::lround(refreshRate * 1000.0)), false, true}
            );
            out.currentModeIndex = 0;
          }
          const auto current = static_cast<std::size_t>(out.currentModeIndex);
          out.modeStr = out.modes[current].width != 0 ? std::to_string(out.modes[current].width)
                  + "x"
                  + std::to_string(out.modes[current].height)
                  + "@"
                  + formatRateHz(out.modes[current].refreshMhz)
                                                      : "";
          outputs.push_back(std::move(out));
        }
      } catch (const std::exception& e) {
        error = e.what();
      }
      return outputs;
    }

  } // namespace

  DisplayBackendSpec hyprlandSpec() {
    return DisplayBackendSpec{
        .kind = "hyprland",
        .supportsHdr = true,
        .fetchArgs = []() { return std::vector<std::string>{"hyprctl", "monitors", "all", "-j"}; },
        .parseFetch = [](std::string_view payload, std::string& error) { return parseFetchJson(payload, error); },
        .composeAll = [](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (!cfg.enabled) {
              cmds.push_back(DisplayCommand{{"hyprctl", "keyword", "monitor", name + ",disable"}, std::nullopt});
              continue;
            }
            cmds.push_back(DisplayCommand{monitorKeyword(name, cfg), std::nullopt});
            cmds.push_back(hdrCommand(name, cfg));
          }
          return cmds;
        },
        .composePositions = [](const std::map<std::string, OutputState>& target) -> std::vector<DisplayCommand> {
          std::vector<DisplayCommand> cmds;
          for (const auto& [name, cfg] : target) {
            if (cfg.enabled) {
              cmds.push_back(DisplayCommand{monitorKeyword(name, cfg), std::nullopt});
            }
          }
          return cmds;
        },
    };
  }

} // namespace compositors::display
