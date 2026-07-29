#include "theme/json_output.h"

#include "theme/palette.h"
#include "theme/tokens.h"

#include <cstdio>
#include <nlohmann/json.hpp>

namespace noctalia::theme {

  namespace {

    std::string hexString(uint32_t argb) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "#%06x", argb & 0x00ffffffU);
      return std::string(buf);
    }

    nlohmann::ordered_json tokenMap(const std::unordered_map<std::string, uint32_t>& tokens) {
      nlohmann::ordered_json out = nlohmann::ordered_json::object();
      for (size_t i = 0; i < kTokenCount; ++i) {
        const std::string key(kTokens[i]);
        auto it = tokens.find(key);
        if (it != tokens.end()) {
          out[key] = hexString(it->second);
        } else {
          out[key] = nullptr;
        }
      }
      return out;
    }

  } // namespace

  std::string toJson(const GeneratedPalette& palette, Scheme /*scheme*/, Variant variant) {
    if (variant == Variant::Dark)
      return tokenMap(palette.dark).dump(2);
    if (variant == Variant::Light)
      return tokenMap(palette.light).dump(2);
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    root["dark"] = tokenMap(palette.dark);
    root["light"] = tokenMap(palette.light);
    return root.dump(2);
  }

} // namespace noctalia::theme
