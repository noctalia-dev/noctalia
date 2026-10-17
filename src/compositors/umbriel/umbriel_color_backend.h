#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>

namespace compositors::umbriel {

  class UmbrielRuntime;

  // The part of the compositor colour state that brightness needs.
  struct OutputColorState {
    std::string hdrMode; // "off", "on", "auto" or "fullscreen"
    bool hdrActive = false;
    float sdrWhite = 0.0F;
  };

  using ColorStates = std::unordered_map<std::string, OutputColorState>;

  // An output that can render HDR has to be brightness controlled even while it does not, because it
  // can switch to HDR at any time and has to carry the level it switches to.
  [[nodiscard]] bool controlsBrightness(const OutputColorState& state) noexcept;
  [[nodiscard]] bool rendersHdrNow(const OutputColorState& state) noexcept;

  [[nodiscard]] ColorStates parseColorStates(const nlohmann::json& ok);
  // Empty when the session has no compositor colour manager.
  [[nodiscard]] ColorStates requestColorStates(const UmbrielRuntime& runtime);

  // The linear map between a 0.0-1.0 brightness and the SDR reference white it selects.
  [[nodiscard]] float sdrWhiteFromBrightness(float brightness) noexcept;
  [[nodiscard]] float brightnessFromSdrWhite(float whiteLevel) noexcept;

  // An empty connector targets every output.
  [[nodiscard]] bool setOutputBrightness(const UmbrielRuntime& runtime, std::string_view connector, float brightness);

} // namespace compositors::umbriel
