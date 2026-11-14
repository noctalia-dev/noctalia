#include "render/text/glyph_registry.h"

#include "core/files/resource_paths.h"
#include "core/log.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace {

  constexpr Logger kLog("glyph");
  constexpr char32_t kMissingGlyph = 0xF292; // skull

  // Hand-curated alias -> native Tabler icon name map.
  // Use these for semantic shell states and stable Noctalia-facing names.
  // clang-format off
const std::unordered_map<std::string, std::string_view> kAliases = {
    // General
    {"close", "x"},
    {"add", "plus"},
    {"more-vertical", "dots-vertical"},
    {"person", "user"},
    {"info", "file-description"},
    {"unpin", "pinned-off"},
    {"image", "photo"},
    {"capslock", "keyboard"},
    {"numlock", "keyboard"},
    {"scrolllock", "keyboard"},
    {"plugin", "plug-connected"},
    {"official-plugin", "shield-filled"},
    {"package", "box"},

    // Toast / warnings
    {"toast-notice", "circle-check"},
    {"toast-warning", "alert-circle"},
    {"toast-error", "circle-x"},
    {"warning", "exclamation-circle"},

    // Media
    {"media-pause", "player-pause-filled"},
    {"media-play", "player-play-filled"},
    {"media-prev", "player-skip-back-filled"},
    {"media-next", "player-skip-forward-filled"},
    {"shuffle", "arrows-shuffle"},
    {"stop", "player-stop-filled"},
    {"microphone-mute", "microphone-off"},

    // Volume
    {"volume-high", "volume"},
    {"volume-low", "volume-2"},
    {"volume-mute", "volume-off"},
    {"volume-x", "volume-3"},
    {"volume-zero", "volume-3"},

    // Network speed
    {"download-speed", "download"},
    {"upload-speed", "upload"},

    // System monitor
    {"cpu-intensive", "alert-octagon"},
    {"cpu-usage", "brand-speedtest"},
    {"cpu-temperature", "flame"},
    {"gpu-usage", "device-desktop"},
    {"memory", "cpu"},
    {"storage", "database"},
    {"busy", "hourglass-empty"},

    // Power
    {"performance", "gauge"},
    {"balanced", "scale"},
    {"powersaver", "leaf"},
    {"shutdown", "power"},
    {"reboot", "refresh"},
    {"reboot-uefi", "device-desktop-cog"},
    {"suspend", "player-pause"},
    {"hibernate", "zzz"},

    // Night light / dark mode
    {"nightlight-on", "moon"},
    {"nightlight-off", "moon-off"},
    {"nightlight-forced", "moon-stars"},
    {"theme-mode", "contrast-filled"},

    // Caffeine (idle inhibitor)
    {"caffeine-on", "mug-filled"},
    {"caffeine-off", "mug"},

    // Brightness / Display
    {"brightness-low", "brightness-down-filled"},
    {"brightness-high", "brightness-up-filled"},

    // Wallpaper / color
    {"wallpaper-selector", "library-photo"},

    // Battery
    {"battery-0", "battery"},
    {"battery-plugged", "battery-charging-2"},

    // Bluetooth devices
    {"bluetooth-device-generic", "bluetooth"},
    {"bluetooth-device-gamepad", "device-gamepad-2"},
    {"bluetooth-device-microphone", "microphone"},
    {"bluetooth-device-headset", "headset"},
    {"bluetooth-device-earbuds", "device-airpods"},
    {"bluetooth-device-headphones", "headphones"},
    {"bluetooth-device-mouse", "mouse-2"},
    {"bluetooth-device-keyboard", "bluetooth"},
    {"bluetooth-device-phone", "device-mobile"},
    {"bluetooth-device-watch", "device-watch"},
    {"bluetooth-device-speaker", "device-speaker"},
    {"bluetooth-device-tv", "device-tv"},

    // Weather
    {"weather-sun", "sun"},
    {"weather-moon", "moon"},
    {"weather-moon-stars", "moon-stars"},
    {"weather-cloud", "cloud"},
    {"weather-cloud-off", "cloud-off"},
    {"weather-cloud-haze", "cloud-fog"},
    {"weather-cloud-lightning", "cloud-bolt"},
    {"weather-cloud-rain", "cloud-rain"},
    {"weather-cloud-snow", "cloud-snow"},
    {"weather-cloud-sun", "cloud-sun"},
    {"weather-sunrise", "sunrise"},
    {"weather-sunset", "sunset"},
};
  // clang-format on

  std::optional<char32_t> parseCodepointLiteral(std::string_view value) {
    if (value.size() < 3) {
      return std::nullopt;
    }

    const bool unicodePrefix = (value[0] == 'U' || value[0] == 'u') && value[1] == '+';
    const bool hexPrefix = value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    if (!unicodePrefix && !hexPrefix) {
      return std::nullopt;
    }
    const std::string_view hex = value.substr(2);

    if (hex.empty()) {
      return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    const auto* begin = hex.data();
    const auto* end = begin + hex.size();
    const auto result = std::from_chars(begin, end, codepoint, 16);
    if (result.ec != std::errc{} || result.ptr != end || codepoint == 0 || codepoint > 0x10FFFF) {
      return std::nullopt;
    }
    return static_cast<char32_t>(codepoint);
  }

  // Calls `add(name, codepoint, category)` for every valid entry of tabler.json.
  template <class Add> void readTablerCatalog(Add&& add) {
    const std::filesystem::path path = paths::libraryAssetPath("fonts/tabler.json");
    std::ifstream file(path);
    if (!file.is_open()) {
      kLog.warn("failed to open Tabler glyph metadata: {}", path.string());
      return;
    }

    try {
      const auto root = nlohmann::json::parse(file);
      if (!root.is_object()) {
        kLog.warn("Tabler glyph metadata is not an object: {}", path.string());
        return;
      }

      std::size_t count = 0;
      for (const auto& [name, value] : root.items()) {
        if (!value.is_object()) {
          continue;
        }
        const auto codepointIt = value.find("codepoint");
        const auto categoryIt = value.find("category");
        if (codepointIt == value.end()
            || categoryIt == value.end()
            || !codepointIt->is_string()
            || !categoryIt->is_string()) {
          continue;
        }
        if (auto parsed = parseCodepointLiteral(codepointIt->get_ref<const std::string&>())) {
          add(name, *parsed, categoryIt->get_ref<const std::string&>());
          ++count;
        }
      }
      kLog.debug("loaded {} Tabler glyph names from {}", count, path.string());
    } catch (const nlohmann::json::exception& e) {
      kLog.warn("failed to parse Tabler glyph metadata '{}': {}", path.string(), e.what());
    }
  }

  // Loaded only for callers that browse the catalog; name lookups use tablerIcons().
  const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata>& tablerMetadata() {
    static const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata> icons = [] {
      std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata> metadata;
      readTablerCatalog([&metadata](const std::string& name, char32_t codepoint, const std::string& category) {
        metadata.emplace(name, GlyphRegistry::TablerGlyphMetadata{.codepoint = codepoint, .category = category});
      });
      return metadata;
    }();
    return icons;
  }

  const std::unordered_map<std::string, char32_t>& tablerIcons() {
    static const std::unordered_map<std::string, char32_t> icons = [] {
      std::unordered_map<std::string, char32_t> flat;
      readTablerCatalog([&flat](const std::string& name, char32_t codepoint, const std::string& /*category*/) {
        flat.emplace(name, codepoint);
      });
      return flat;
    }();
    return icons;
  }

} // namespace

bool GlyphRegistry::contains(std::string_view name) {
  if (parseCodepointLiteral(name).has_value()) {
    return true;
  }

  const auto& tabler = tablerIcons();
  const std::string key{name};
  if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
    return tabler.contains(std::string(alias->second));
  }
  return tabler.contains(key);
}

char32_t GlyphRegistry::lookup(std::string_view name) {
  if (auto codepoint = parseCodepointLiteral(name)) {
    return *codepoint;
  }

  const auto& tabler = tablerIcons();
  const std::string key{name};
  if (const auto alias = kAliases.find(key); alias != kAliases.end()) {
    const auto it = tabler.find(std::string(alias->second));
    if (it != tabler.end()) {
      return it->second;
    }

    kLog.warn("missing Tabler glyph '{}' for alias '{}'", alias->second, name);
    return kMissingGlyph;
  }

  const auto it = tabler.find(key);
  if (it != tabler.end()) {
    return it->second;
  }

  kLog.warn("missing glyph: {}", name);
  return kMissingGlyph;
}

const std::unordered_map<std::string, GlyphRegistry::TablerGlyphMetadata>& GlyphRegistry::tablerGlyphMetadata() {
  return ::tablerMetadata();
}

const std::unordered_map<std::string, std::string_view>& GlyphRegistry::aliases() { return kAliases; }
