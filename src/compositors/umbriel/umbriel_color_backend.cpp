#include "compositors/umbriel/umbriel_color_backend.h"

#include "compositors/umbriel/umbriel_runtime.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr float kSdrWhiteMin = 80.0F;
  constexpr float kSdrWhiteMax = 203.0F;

  [[nodiscard]] bool rendersHdr(std::string_view hdrMode) {
    return hdrMode == "on" || hdrMode == "auto" || hdrMode == "fullscreen";
  }

  [[nodiscard]] compositors::umbriel::ColorStates parseOutputs(const nlohmann::json& ok) {
    compositors::umbriel::ColorStates states;
    const auto outputs = ok.find("outputs");
    if (outputs == ok.end() || !outputs->is_array()) {
      return states;
    }

    for (const auto& output : *outputs) {
      if (!output.is_object()) {
        continue;
      }

      const auto name = output.find("name");
      const auto hdrMode = output.find("hdr_mode");
      if (name == output.end()
          || !name->is_string()
          || name->get_ref<const std::string&>().empty()
          || hdrMode == output.end()
          || !hdrMode->is_string()) {
        continue;
      }

      compositors::umbriel::OutputColorState state;
      state.hdrMode = hdrMode->get<std::string>();
      if (const auto hdrActive = output.find("hdr_active"); hdrActive != output.end() && hdrActive->is_boolean()) {
        state.hdrActive = hdrActive->get<bool>();
      }
      const auto sdrWhite = output.find("sdr_white");
      if (sdrWhite != output.end() && sdrWhite->is_number()) {
        state.sdrWhite = sdrWhite->get<float>();
      }
      states.emplace(name->get<std::string>(), std::move(state));
    }
    return states;
  }

} // namespace

namespace compositors::umbriel {

  bool controlsBrightness(const OutputColorState& state) noexcept { return rendersHdr(state.hdrMode); }

  bool rendersHdrNow(const OutputColorState& state) noexcept { return state.hdrActive; }

  ColorStates parseColorStates(const nlohmann::json& ok) {
    if (!ok.is_object()) {
      return {};
    }
    return parseOutputs(ok);
  }

  ColorStates requestColorStates(const UmbrielRuntime& runtime) {
    const auto response = runtime.requestCommand("color");
    return response ? parseColorStates(response->at("ok")) : ColorStates{};
  }

  float sdrWhiteFromBrightness(float brightness) noexcept {
    const float value = std::clamp(brightness, 0.0F, 1.0F);
    return kSdrWhiteMin + value * (kSdrWhiteMax - kSdrWhiteMin);
  }

  float brightnessFromSdrWhite(float whiteLevel) noexcept {
    return std::clamp((whiteLevel - kSdrWhiteMin) / (kSdrWhiteMax - kSdrWhiteMin), 0.0F, 1.0F);
  }

  bool setOutputBrightness(const UmbrielRuntime& runtime, std::string_view connector, float brightness) {
    // Rounded, so a written level reads back as the percentage it was written from.
    nlohmann::json fields{{"nits", std::round(sdrWhiteFromBrightness(brightness) * 10.0) / 10.0}};
    if (!connector.empty()) {
      fields["output"] = std::string(connector);
    }
    return runtime.requestAction("output-sdr-white", std::move(fields));
  }

} // namespace compositors::umbriel
