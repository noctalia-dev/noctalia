#include "theme/custom_palettes.h"

#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <json.hpp>
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
    std::sort(out.begin(), out.end(), [](const AvailablePalette& a, const AvailablePalette& b) {
      return a.name < b.name;
    });
    return out;
  }

} // namespace noctalia::theme
