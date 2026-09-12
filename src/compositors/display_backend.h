#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace compositors {

  class CompositorRuntimeRegistry;
  namespace sway {
    class SwayRuntime;
  }
  namespace mango {
    class MangoRuntime;
  }

  namespace display {

    struct DisplayMode {
      int width = 0;
      int height = 0;
      int refreshMhz = 60000;
      bool preferred = false;
      bool current = false;
    };

    // Per-output features (what the display offers) and settings (what is applied).
    struct OutputState {
      std::string name;
      bool enabled = true;
      std::string make;
      std::string model;
      std::string modeStr;
      double scale = 1.0;
      std::string transform = "Normal";
      int x = 0;
      int y = 0;
      bool vrr = false;
      bool hdr = false;
      bool hdrSupported = false;
      std::vector<DisplayMode> modes;
      int currentModeIndex = 0;
    };

    struct DisplayCommand {
      std::vector<std::string> args;
      std::optional<int> sleepMs;
      std::function<void()> action;
    };

    struct OutputDiff {
      bool enabled = false;
      bool mode = false;
      bool scale = false;
      bool transform = false;
      bool position = false;
      bool vrr = false;
      bool hdr = false;

      [[nodiscard]] bool any() const { return enabled || mode || scale || transform || position || vrr || hdr; }
    };

    [[nodiscard]] OutputDiff diffOutputs(const OutputState& snapshot, const OutputState& current);

    // Per-compositor adapter: composes the command set from the OutputState
    // structure. Everything else (queue, snapshots, revert, UI) is shared.
    struct DisplayBackendSpec {
      std::string kind;
      bool writable = true;
      bool supportsHdr = false;
      std::function<std::vector<std::string>()> fetchArgs; // empty = in-process fetch
      std::function<std::vector<OutputState>(std::string_view payload, std::string& error)> parseFetch;
      // Full settings for every output in the map.
      std::function<std::vector<DisplayCommand>(const std::map<std::string, OutputState>&)> composeAll;
      // Position-only commands (drag path); may equal composeAll for full-rebuild adapters.
      std::function<std::vector<DisplayCommand>(const std::map<std::string, OutputState>&)> composePositions;
    };

    class DisplayBackend {
    public:
      explicit DisplayBackend(DisplayBackendSpec spec);

      [[nodiscard]] const std::vector<std::string>& fetchArgs() const { return m_fetchArgs; }
      [[nodiscard]] std::vector<OutputState> parseFetch(std::string_view payload, std::string& error) const {
        return m_spec.parseFetch ? m_spec.parseFetch(payload, error) : std::vector<OutputState>{};
      }
      [[nodiscard]] std::vector<DisplayCommand> applyCommands(const std::map<std::string, OutputState>& target) const {
        return m_spec.composeAll ? m_spec.composeAll(target) : std::vector<DisplayCommand>{};
      }
      [[nodiscard]] std::vector<DisplayCommand>
      positionsCommands(const std::map<std::string, OutputState>& target) const {
        return m_spec.composePositions ? m_spec.composePositions(target) : std::vector<DisplayCommand>{};
      }
      [[nodiscard]] std::vector<DisplayCommand> revertCommands(
          const std::map<std::string, OutputState>& snapshot, const std::map<std::string, OutputState>& current
      ) const {
        for (const auto& [name, snap] : snapshot) {
          const auto it = current.find(name);
          const OutputState& cur = it != current.end() ? it->second : OutputState{};
          if (diffOutputs(snap, cur).any()) {
            return applyCommands(snapshot);
          }
        }
        return {};
      }
      [[nodiscard]] bool writable() const { return m_spec.writable; }
      [[nodiscard]] const std::string& kindName() const { return m_spec.kind; }
      [[nodiscard]] bool supportsHdr() const { return m_spec.supportsHdr; }

    private:
      DisplayBackendSpec m_spec;
      std::vector<std::string> m_fetchArgs;
    };

    [[nodiscard]] std::unique_ptr<DisplayBackend> createDisplayBackend(CompositorRuntimeRegistry& runtimeRegistry);

    [[nodiscard]] DisplayBackendSpec hyprlandSpec();
    [[nodiscard]] DisplayBackendSpec niriSpec();
    [[nodiscard]] DisplayBackendSpec swaySpec(compositors::sway::SwayRuntime& runtime);
    [[nodiscard]] DisplayBackendSpec wlrSpec(bool readonly);
    [[nodiscard]] DisplayBackendSpec mangoSpec(compositors::mango::MangoRuntime& runtime);

    // Canonical name -> backend spelling; index is also the wl_output transform code.
    inline const std::array<std::pair<std::string_view, std::string_view>, 8> kTransforms{{
        {"Normal", "normal"},
        {"90", "90"},
        {"180", "180"},
        {"270", "270"},
        {"Flipped", "flipped"},
        {"Flipped90", "flipped-90"},
        {"Flipped180", "flipped-180"},
        {"Flipped270", "flipped-270"},
    }};

    [[nodiscard]] std::string_view transformToName(std::string_view transform);
    [[nodiscard]] std::string transformFromName(std::string_view name);
    [[nodiscard]] std::string transformFromCode(int code);
    [[nodiscard]] int transformToCode(std::string_view transform);
    [[nodiscard]] std::string swayModeStr(std::string_view modeStr);
    [[nodiscard]] std::string formatRateHz(int refreshMhz);
    [[nodiscard]] bool rotatedTransform(std::string_view transform);

  } // namespace display

} // namespace compositors
