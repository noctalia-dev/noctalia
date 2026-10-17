#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::umbriel {

  class UmbrielRuntime;

  struct OutputColorState {
    std::string hdrMode; // "off", "on", "auto" or "fullscreen"
    bool hdrActive = false;
    float sdrWhite = 0.0F;
  };

  using ColorStates = std::unordered_map<std::string, OutputColorState>;

  // Any mode that can render HDR, not only the output rendering it right now.
  [[nodiscard]] bool controlsBrightness(const OutputColorState& state) noexcept;
  [[nodiscard]] bool rendersHdrNow(const OutputColorState& state) noexcept;

  [[nodiscard]] ColorStates parseColorStates(const nlohmann::json& ok);
  // Empty when the session has no compositor colour manager.
  [[nodiscard]] ColorStates requestColorStates(const UmbrielRuntime& runtime);

  [[nodiscard]] float sdrWhiteFromBrightness(float brightness) noexcept;
  [[nodiscard]] float brightnessFromSdrWhite(float whiteLevel) noexcept;

  // An empty connector selects the compositor's focused output.
  [[nodiscard]] bool setOutputBrightness(const UmbrielRuntime& runtime, std::string_view connector, float brightness);

} // namespace compositors::umbriel
