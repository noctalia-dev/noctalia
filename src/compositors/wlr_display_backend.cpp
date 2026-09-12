#include "compositors/display_backend.h"
#include "core/process/process.h"
#include "util/string_utils.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace compositors::display {

  namespace {

    [[nodiscard]] std::vector<std::string> fullWlrCmd(const std::map<std::string, OutputState>& target) {
      std::vector<std::string> args{"wlr-randr"};
      for (const auto& [name, cfg] : target) {
        args.push_back("--output");
        args.push_back(name);
        if (!cfg.enabled) {
          args.push_back("--off");
          continue;
        }
        args.push_back("--on");
        if (!cfg.modeStr.empty()) {
          args.push_back("--mode");
          args.push_back(cfg.modeStr);
        }
        args.push_back("--scale");
        args.push_back(StringUtils::formatDotDecimal(cfg.scale));
        args.push_back("--transform");
        args.push_back(std::string(transformToName(cfg.transform)));
        args.push_back("--pos");
        args.push_back(std::to_string(cfg.x) + "," + std::to_string(cfg.y));
        args.push_back("--adaptive-sync");
        args.push_back(cfg.vrr ? "enabled" : "disabled");
      }
      return args;
    }

    [[nodiscard]] std::vector<OutputState> parseWlrJson(std::string_view payload, std::string& error) {
      std::vector<OutputState> outputs;
      try {
        const auto json = nlohmann::json::parse(payload);
        if (!json.is_array()) {
          error = "unexpected wlr-randr payload";
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
          out.enabled = mon.value("enabled", true);
          out.make = mon.value("make", "");
          out.model = mon.value("model", "");
          out.vrr = mon.value("adaptive_sync", false);
          int currentW = 1920;
          int currentH = 1080;
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
              if (const auto r = mode.find("refresh"); r != mode.end() && r->is_number()) {
                refreshMhz = static_cast<int>(std::lround(r->get<double>() * 1000.0));
              }
              DisplayMode entry{w->get<int>(), h->get<int>(), refreshMhz, false, false};
              if (mode.value("current", false)) {
                entry.current = true;
                out.currentModeIndex = static_cast<int>(out.modes.size());
                currentW = entry.width;
                currentH = entry.height;
              }
              out.modes.push_back(entry);
            }
          }
          if (out.modes.empty()) {
            out.modes.push_back(DisplayMode{currentW, currentH, 60000, false, true});
            out.currentModeIndex = 0;
          }
          const auto current = static_cast<std::size_t>(out.currentModeIndex);
          out.modeStr = std::to_string(out.modes[current].width)
              + "x"
              + std::to_string(out.modes[current].height)
              + "@"
              + formatRateHz(out.modes[current].refreshMhz);
          out.scale = mon.value("scale", 1.0);
          if (out.scale <= 0.0) {
            out.scale = 1.0;
          }
          out.transform = transformFromName(mon.value("transform", "normal"));
          if (const auto position = mon.find("position"); position != mon.end() && position->is_object()) {
            out.x = position->value("x", 0);
            out.y = position->value("y", 0);
          }
          outputs.push_back(std::move(out));
        }
      } catch (const std::exception& e) {
        error = e.what();
      }
      return outputs;
    }

    [[nodiscard]] std::vector<OutputState> readDrmSysfs(std::string& error) {
      std::vector<OutputState> outputs;
      const std::filesystem::path drmDir("/sys/class/drm");
      std::error_code ec;
      if (!std::filesystem::is_directory(drmDir, ec)) {
        error = "/sys/class/drm is not available";
        return outputs;
      }
      for (const auto& entry : std::filesystem::directory_iterator(drmDir, ec)) {
        const auto filename = entry.path().filename().string();
        if (!filename.starts_with("card")) {
          continue;
        }
        const auto dash = filename.find('-');
        if (dash == std::string::npos) {
          continue;
        }
        std::ifstream status(entry.path() / "status");
        std::string statusValue;
        std::getline(status, statusValue);
        if (StringUtils::trim(statusValue) != "connected") {
          continue;
        }
        std::ifstream modes(entry.path() / "modes");
        std::string modeLine;
        std::getline(modes, modeLine);
        const auto mode = StringUtils::trim(modeLine);
        int width = 0;
        int height = 0;
        const auto xPos = mode.find('x');
        if (xPos != std::string::npos && !mode.empty() && mode[0] >= '0' && mode[0] <= '9') {
          width = std::stoi(mode.substr(0, xPos));
          height = std::stoi(mode.substr(xPos + 1));
        }
        OutputState out;
        out.name = filename.substr(dash + 1);
        out.enabled = true;
        out.modeStr = width > 0 ? std::to_string(width) + "x" + std::to_string(height) + "@60.000" : "";
        out.modes = {DisplayMode{width > 0 ? width : 1920, height > 0 ? height : 1080, 60000, false, true}};
        outputs.push_back(std::move(out));
      }
      return outputs;
    }

  } // namespace

  DisplayBackendSpec wlrSpec(bool readonly) {
    DisplayBackendSpec spec;
    spec.kind = readonly ? "readonly" : "wlroots";
    spec.writable = !readonly;
    if (readonly) {
      spec.fetchArgs = []() { return std::vector<std::string>{}; };
      spec.parseFetch = [](std::string_view /*payload*/, std::string& error) { return readDrmSysfs(error); };
      spec.composeAll = [](const std::map<std::string, OutputState>&) { return std::vector<DisplayCommand>{}; };
      spec.composePositions = [](const std::map<std::string, OutputState>&) { return std::vector<DisplayCommand>{}; };
      return spec;
    }
    spec.fetchArgs = []() { return std::vector<std::string>{"wlr-randr", "--json"}; };
    spec.parseFetch = [](std::string_view payload, std::string& error) { return parseWlrJson(payload, error); };
    spec.composeAll = [](const std::map<std::string, OutputState>& target) {
      return std::vector<DisplayCommand>{DisplayCommand{fullWlrCmd(target), std::nullopt}};
    };
    spec.composePositions = [](const std::map<std::string, OutputState>& target) {
      return std::vector<DisplayCommand>{DisplayCommand{fullWlrCmd(target), std::nullopt}};
    };
    return spec;
  }

} // namespace compositors::display
