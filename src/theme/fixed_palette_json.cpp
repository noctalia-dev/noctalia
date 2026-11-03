#include "theme/fixed_palette_json.h"

#include "core/log.h"
#include "render/core/color.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr auto kLog = Logger("theme");

    // Returns transparent black if the key is missing or the value is not a hex string.
    ::Color readColorField(const nlohmann::json& obj, std::string_view key) {
      const auto it = obj.find(key);
      if (it == obj.end() || !it->is_string()) {
        return ::Color{};
      }
      try {
        return hex(it->get<std::string>());
      } catch (const std::exception&) {
        return ::Color{};
      }
    }

    FixedPalette readPaletteJson(const nlohmann::json& obj) {
      FixedPalette palette{};
      for (const auto& [key, field] : kFixedPaletteJsonKeys) {
        palette.*field = readColorField(obj, key);
      }
      return palette;
    }

    struct Version1Key {
      std::string_view current;
      // Format 1 spelling without its optional `m` prefix.
      std::string_view version1;
    };

    constexpr std::array<Version1Key, 16> kVersion1RoleKeys{{
        {"primary", "primary"},
        {"on_primary", "onPrimary"},
        {"secondary", "secondary"},
        {"on_secondary", "onSecondary"},
        {"tertiary", "tertiary"},
        {"on_tertiary", "onTertiary"},
        {"error", "error"},
        {"on_error", "onError"},
        {"surface", "surface"},
        {"on_surface", "onSurface"},
        {"surface_container", "surfaceVariant"},
        {"on_surface_variant", "onSurfaceVariant"},
        {"outline_variant", "outline"},
        {"shadow", "shadow"},
        {"hover", "hover"},
        {"on_hover", "onHover"},
    }};

    constexpr std::array<Version1Key, 6> kVersion1TerminalKeys{{
        {"foreground", "foreground"},
        {"background", "background"},
        {"cursor", "cursor"},
        {"cursor_text", "cursorText"},
        {"selection_fg", "selectionFg"},
        {"selection_bg", "selectionBg"},
    }};

    // Format 1 readers took `m` + the capitalized name first (`mOnPrimary`), then the bare name (`onPrimary`).
    void renameVersion1Key(nlohmann::json& obj, std::string_view current, std::string_view version1) {
      std::string prefixed = "m";
      prefixed.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(version1.front()))));
      prefixed.append(version1.substr(1));
      std::optional<nlohmann::json> value;
      for (const std::string& name : {prefixed, std::string(version1)}) {
        if (auto it = obj.find(name); it != obj.end()) {
          if (!value) {
            value = std::move(*it);
          }
          obj.erase(it);
        }
      }
      // The current name may carry an unrelated value in format 1 (some files had an unused mOutlineVariant).
      obj.erase(std::string(current));
      if (value) {
        obj[std::string(current)] = std::move(*value);
      }
    }

    TerminalAnsiColors readAnsiJson(const nlohmann::json& obj) {
      return TerminalAnsiColors{
          .black = readColorField(obj, kTerminalBlackJsonKey),
          .red = readColorField(obj, kTerminalRedJsonKey),
          .green = readColorField(obj, kTerminalGreenJsonKey),
          .yellow = readColorField(obj, kTerminalYellowJsonKey),
          .blue = readColorField(obj, kTerminalBlueJsonKey),
          .magenta = readColorField(obj, kTerminalMagentaJsonKey),
          .cyan = readColorField(obj, kTerminalCyanJsonKey),
          .white = readColorField(obj, kTerminalWhiteJsonKey),
      };
    }

    TerminalPalette readTerminalJson(const nlohmann::json& obj) {
      TerminalPalette tp{};
      if (auto it = obj.find(kTerminalNormalJsonKey); it != obj.end() && it->is_object()) {
        tp.normal = readAnsiJson(*it);
      }
      if (auto it = obj.find(kTerminalBrightJsonKey); it != obj.end() && it->is_object()) {
        tp.bright = readAnsiJson(*it);
      }
      tp.foreground = readColorField(obj, kTerminalForegroundJsonKey);
      tp.background = readColorField(obj, kTerminalBackgroundJsonKey);
      tp.selectionFg = readColorField(obj, kTerminalSelectionFgJsonKey);
      tp.selectionBg = readColorField(obj, kTerminalSelectionBgJsonKey);
      tp.cursorText = readColorField(obj, kTerminalCursorTextJsonKey);
      tp.cursor = readColorField(obj, kTerminalCursorJsonKey);
      return tp;
    }

    std::optional<TerminalPalette> readModeTerminalJson(const nlohmann::json& obj) {
      auto it = obj.find(kTerminalJsonKey);
      if (it == obj.end() || !it->is_object())
        return std::nullopt;
      return readTerminalJson(*it);
    }

  } // namespace

  std::expected<int, std::string> paletteJsonVersion(const nlohmann::json& root) {
    if (!root.is_object()) {
      return 1;
    }
    const auto it = root.find(kPaletteVersionJsonKey);
    if (it == root.end()) {
      return 1;
    }
    if (!it->is_number_integer() || it->get<std::int64_t>() < 1) {
      return std::unexpected(std::string(R"(palette "version" must be a positive integer)"));
    }
    const std::int64_t version = it->get<std::int64_t>();
    if (version > kPaletteFormatVersion) {
      return std::unexpected(
          std::format(
              "palette format version {} is newer than the supported version {}", version, kPaletteFormatVersion
          )
      );
    }
    return static_cast<int>(version);
  }

  void upgradeFixedPaletteModeJson(nlohmann::json& mode, int fromVersion) {
    if (!mode.is_object()) {
      return;
    }
    if (fromVersion < 2) {
      // Format 2 dropped the `m` prefix, spells keys in snake_case after the color roles they feed, and renamed
      // mSurfaceVariant and mOutline to surface_container and outline_variant.
      for (const auto& key : kVersion1RoleKeys) {
        renameVersion1Key(mode, key.current, key.version1);
      }
      if (auto terminal = mode.find(kTerminalJsonKey); terminal != mode.end() && terminal->is_object()) {
        for (const auto& key : kVersion1TerminalKeys) {
          renameVersion1Key(*terminal, key.current, key.version1);
        }
        for (const auto group : {kTerminalNormalJsonKey, kTerminalBrightJsonKey}) {
          if (auto colors = terminal->find(group); colors != terminal->end() && colors->is_object()) {
            for (const auto key : kTerminalAnsiColorJsonKeys) {
              renameVersion1Key(*colors, key, key);
            }
          }
        }
      }
    }
  }

  std::optional<ParsedPaletteFile> parsePaletteFile(const std::filesystem::path& path) {
    try {
      std::ifstream in(path);
      if (!in)
        return std::nullopt;
      std::stringstream buf;
      buf << in.rdbuf();
      auto root = nlohmann::json::parse(buf.str());
      if (!root.is_object())
        return std::nullopt;
      const auto version = paletteJsonVersion(root);
      if (!version) {
        kLog.warn("palette '{}': {}", path.string(), version.error());
        return std::nullopt;
      }
      ParsedPaletteFile out{};
      if (auto it = root.find("dark"); it != root.end() && it->is_object()) {
        upgradeFixedPaletteModeJson(*it, *version);
        out.dark.palette = readPaletteJson(*it);
        if (auto terminal = readModeTerminalJson(*it)) {
          out.dark.terminal = *terminal;
        } else {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
      if (auto it = root.find("light"); it != root.end() && it->is_object()) {
        upgradeFixedPaletteModeJson(*it, *version);
        out.light.palette = readPaletteJson(*it);
        if (auto terminal = readModeTerminalJson(*it)) {
          out.light.terminal = *terminal;
        } else {
          return std::nullopt;
        }
      } else {
        out.light = out.dark;
      }
      return out;
    } catch (const std::exception& e) {
      kLog.warn("failed to parse community palette '{}': {}", path.string(), e.what());
      return std::nullopt;
    }
  }

} // namespace noctalia::theme
