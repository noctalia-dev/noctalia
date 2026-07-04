#include "theme/custom_palettes.h"

#include "theme/fixed_palette.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace noctalia::theme {

  namespace {

    std::string stringField(const nlohmann::json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    std::string fixedPaletteModeColorField(const nlohmann::json& obj, std::string_view camelField) {
      std::string prefixed = "m";
      prefixed.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(camelField.front()))));
      prefixed.append(camelField.substr(1));
      if (std::string color = stringField(obj, prefixed); !color.empty()) {
        return color;
      }
      return stringField(obj, camelField);
    }

    AvailablePalette::PreviewMode palettePreviewFromModeJson(const nlohmann::json& modeJson) {
      if (!modeJson.is_object()) {
        return {};
      }

      static constexpr auto kPreviewFields = std::to_array<std::string_view>({
          "primary",
          "secondary",
          "tertiary",
          "error",
      });

      AvailablePalette::PreviewMode preview;
      preview.accents.reserve(kPreviewFields.size());
      preview.surface = fixedPaletteModeColorField(modeJson, "surface");
      for (const auto field : kPreviewFields) {
        std::string color = fixedPaletteModeColorField(modeJson, field);
        if (!color.empty()) {
          preview.accents.push_back(std::move(color));
        }
      }
      return preview;
    }

    AvailablePalette::Preview palettePreviewFromFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      if (!in) {
        return {};
      }

      try {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto root = nlohmann::json::parse(buf.str());
        if (!root.is_object()) {
          return {};
        }

        AvailablePalette::Preview preview;
        if (auto it = root.find("dark"); it != root.end() && it->is_object()) {
          preview.dark = palettePreviewFromModeJson(*it);
        }
        if (auto it = root.find("light"); it != root.end() && it->is_object()) {
          preview.light = palettePreviewFromModeJson(*it);
        }
        if (preview.dark.accents.empty() && preview.light.accents.empty()) {
          preview.dark = palettePreviewFromModeJson(root);
          preview.light = preview.dark;
        } else if (preview.light.accents.empty()) {
          preview.light = preview.dark;
        }
        return preview;
      } catch (const std::exception&) {
        return {};
      }
    }

    std::string renderColorToHex(const ::Color& color) {
      auto toByte = [](float value) { return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
      char buf[8];
      std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", toByte(color.r), toByte(color.g), toByte(color.b));
      return std::string(buf);
    }

    void writeFixedColor(nlohmann::ordered_json& obj, std::string_view camelField, const ::Color& color) {
      std::string key = "m";
      key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(camelField.front()))));
      key.append(camelField.substr(1));
      obj[key] = renderColorToHex(color);
    }

    nlohmann::ordered_json ansiColorsToJson(const TerminalAnsiColors& colors) {
      nlohmann::ordered_json out = nlohmann::ordered_json::object();
      out[kTerminalBlackJsonKey] = renderColorToHex(colors.black);
      out[kTerminalRedJsonKey] = renderColorToHex(colors.red);
      out[kTerminalGreenJsonKey] = renderColorToHex(colors.green);
      out[kTerminalYellowJsonKey] = renderColorToHex(colors.yellow);
      out[kTerminalBlueJsonKey] = renderColorToHex(colors.blue);
      out[kTerminalMagentaJsonKey] = renderColorToHex(colors.magenta);
      out[kTerminalCyanJsonKey] = renderColorToHex(colors.cyan);
      out[kTerminalWhiteJsonKey] = renderColorToHex(colors.white);
      return out;
    }

    nlohmann::ordered_json terminalPaletteToJson(const TerminalPalette& terminal) {
      nlohmann::ordered_json out = nlohmann::ordered_json::object();
      out[kTerminalNormalJsonKey] = ansiColorsToJson(terminal.normal);
      out[kTerminalBrightJsonKey] = ansiColorsToJson(terminal.bright);
      out[kTerminalForegroundJsonKey] = renderColorToHex(terminal.foreground);
      out[kTerminalBackgroundJsonKey] = renderColorToHex(terminal.background);
      out[kTerminalCursorJsonKey] = renderColorToHex(terminal.cursor);
      out[kTerminalCursorTextJsonKey] = renderColorToHex(terminal.cursorText);
      out[kTerminalSelectionFgJsonKey] = renderColorToHex(terminal.selectionFg);
      out[kTerminalSelectionBgJsonKey] = renderColorToHex(terminal.selectionBg);
      return out;
    }

    nlohmann::ordered_json fixedPaletteModeToJson(const TokenMap& tokens) {
      const ::Palette palette = mapGeneratedPaletteMode(tokens);
      const TerminalPalette terminal = terminalPaletteFromTokens(tokens);

      nlohmann::ordered_json out = nlohmann::ordered_json::object();
      writeFixedColor(out, "primary", palette.primary);
      writeFixedColor(out, "onPrimary", palette.onPrimary);
      writeFixedColor(out, "secondary", palette.secondary);
      writeFixedColor(out, "onSecondary", palette.onSecondary);
      writeFixedColor(out, "tertiary", palette.tertiary);
      writeFixedColor(out, "onTertiary", palette.onTertiary);
      writeFixedColor(out, "error", palette.error);
      writeFixedColor(out, "onError", palette.onError);
      writeFixedColor(out, "surface", palette.surface);
      writeFixedColor(out, "onSurface", palette.onSurface);
      writeFixedColor(out, "surfaceVariant", palette.surfaceVariant);
      writeFixedColor(out, "onSurfaceVariant", palette.onSurfaceVariant);
      writeFixedColor(out, "outline", palette.outline);
      writeFixedColor(out, "shadow", palette.shadow);
      writeFixedColor(out, "hover", palette.hover);
      writeFixedColor(out, "onHover", palette.onHover);
      out[std::string(kTerminalJsonKey)] = terminalPaletteToJson(terminal);
      return out;
    }

    std::string sanitizePaletteNameComponent(std::string_view raw) {
      std::string out;
      out.reserve(raw.size());
      bool pendingDash = false;
      for (const char ch : raw) {
        if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
          if (pendingDash && !out.empty()) {
            out.push_back('-');
            pendingDash = false;
          }
          out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (!out.empty()) {
          pendingDash = true;
        }
      }
      while (!out.empty() && out.back() == '-') {
        out.pop_back();
      }
      return out;
    }

    constexpr std::size_t kUnknownSchemeTagMaxLen = 4;

    void truncatePaletteComponent(std::string& value, std::size_t maxLen) {
      if (value.size() <= maxLen) {
        return;
      }
      value.resize(maxLen);
      while (!value.empty() && value.back() == '-') {
        value.pop_back();
      }
    }

    std::string shortSchemeTag(std::string_view scheme) {
      static constexpr std::pair<std::string_view, std::string_view> kTags[] = {
          {"m3-tonal-spot", "ts"},  {"m3-content", "mc"}, {"m3-fruit-salad", "fs"}, {"m3-rainbow", "rb"},
          {"m3-monochrome", "mo"},  {"vibrant", "vib"},   {"faithful", "fth"},      {"soft", "sft"},
          {"dysfunctional", "dys"}, {"muted", "mut"},
      };
      for (const auto& [full, tag] : kTags) {
        if (scheme == full) {
          return std::string(tag);
        }
      }
      std::string fallback = sanitizePaletteNameComponent(scheme);
      truncatePaletteComponent(fallback, kUnknownSchemeTagMaxLen);
      return fallback;
    }

  } // namespace

  std::filesystem::path customPaletteDir() {
    const std::string dir = FileUtils::configDir();
    return dir.empty() ? std::filesystem::path{} : std::filesystem::path(dir) / "palettes";
  }

  std::filesystem::path customPalettePath(std::string_view name) {
    return customPaletteDir() / (std::string(name) + ".json");
  }

  std::vector<AvailablePalette> availableCustomPalettes() {
    const auto dir = customPaletteDir();
    if (dir.empty()) {
      return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
      return {};
    }
    std::vector<AvailablePalette> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (ec || !entry.is_regular_file(ec) || ec) {
        continue;
      }
      const auto& path = entry.path();
      if (path.extension() != ".json") {
        continue;
      }
      out.push_back(AvailablePalette{.name = path.stem().string(), .preview = palettePreviewFromFile(path)});
    }
    std::ranges::sort(out, {}, &AvailablePalette::name);
    return out;
  }

  std::string suggestCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme) {
    std::string base;
    if (wallpaperPath.starts_with("color:")) {
      base = sanitizePaletteNameComponent(wallpaperPath.substr(std::string_view("color:").size()));
      if (base.empty()) {
        base = "solid";
      } else {
        base = "color-" + base;
      }
    } else if (!wallpaperPath.empty()) {
      base = sanitizePaletteNameComponent(std::filesystem::path(wallpaperPath).stem().string());
    }
    if (base.empty()) {
      base = "wallpaper";
    }
    const std::string schemeTag = shortSchemeTag(scheme);
    if (!schemeTag.empty()) {
      base += '-';
      base += schemeTag;
    }
    return base;
  }

  std::string allocateCustomPaletteName(std::string_view wallpaperPath, std::string_view scheme) {
    const std::string base = suggestCustomPaletteName(wallpaperPath, scheme);
    if (base.empty()) {
      return {};
    }
    if (!std::filesystem::exists(customPalettePath(base))) {
      return base;
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
      const std::string candidate = base + '-' + std::to_string(suffix);
      if (!std::filesystem::exists(customPalettePath(candidate))) {
        return candidate;
      }
    }
    return {};
  }

  bool saveCustomPaletteFromGenerated(std::string_view name, const GeneratedPalette& palette, std::string* errorOut) {
    auto fail = [errorOut](std::string message) {
      if (errorOut != nullptr) {
        *errorOut = std::move(message);
      }
      return false;
    };

    const std::string trimmed = sanitizePaletteNameComponent(name);
    if (trimmed.empty()) {
      return fail("invalid custom palette name");
    }

    const auto dir = customPaletteDir();
    if (dir.empty()) {
      return fail("config directory unavailable");
    }

    GeneratedPalette copy = palette;
    synthesizeTerminalPaletteTokens(copy);
    if (copy.dark.empty() && copy.light.empty()) {
      return fail("generated palette is empty");
    }

    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    if (!copy.dark.empty()) {
      root["dark"] = fixedPaletteModeToJson(copy.dark);
    }
    if (!copy.light.empty()) {
      root["light"] = fixedPaletteModeToJson(copy.light);
    }
    if (root.empty()) {
      return fail("generated palette is empty");
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      return fail("failed to create palettes directory");
    }

    const auto path = customPalettePath(trimmed);
    std::ofstream out(path);
    if (!out) {
      return fail("failed to write custom palette file");
    }
    out << root.dump(2);
    out.close();
    if (!out) {
      return fail("failed to write custom palette file");
    }

    const auto preview = palettePreviewFromFile(path);
    if (preview.dark.accents.empty() && preview.light.accents.empty()) {
      std::filesystem::remove(path, ec);
      return fail("saved custom palette failed validation");
    }
    return true;
  }

} // namespace noctalia::theme
