#include "render/text/system_font.h"

#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <gio/gio.h>
#include <memory>
#include <optional>
#include <pango/pango.h>
#include <system_error>

namespace text {

  namespace {

    constexpr const char* kInterfaceSchema = "org.gnome.desktop.interface";
    constexpr float kPointsToPx = 96.0F / 72.0F;
    constexpr float kMinTextScalingFactor = 0.5F;
    constexpr float kMaxTextScalingFactor = 3.0F;

    struct GSettingsDeleter {
      void operator()(GSettings* settings) const { g_object_unref(settings); }
    };

    struct InterfaceSettings {
      std::unique_ptr<GSettings, GSettingsDeleter> settings;
      bool hasFontName = false;
      bool hasTextScalingFactor = false;
    };

    const InterfaceSettings& interfaceSettings() {
      static const InterfaceSettings s = [] {
        InterfaceSettings out;
        GSettingsSchemaSource* source = g_settings_schema_source_get_default();
        if (source == nullptr) {
          return out;
        }
        GSettingsSchema* schema = g_settings_schema_source_lookup(source, kInterfaceSchema, TRUE);
        if (schema == nullptr) {
          return out;
        }
        out.hasFontName = g_settings_schema_has_key(schema, "font-name") != FALSE;
        out.hasTextScalingFactor = g_settings_schema_has_key(schema, "text-scaling-factor") != FALSE;
        g_settings_schema_unref(schema);
        if (out.hasFontName || out.hasTextScalingFactor) {
          out.settings.reset(g_settings_new(kInterfaceSchema));
        }
        return out;
      }();
      return s;
    }

    // The user-set value only: the schema default would always win over the built-in fallback size.
    std::optional<std::string> readGSettingsFontName() {
      const auto& s = interfaceSettings();
      if (s.settings == nullptr || !s.hasFontName) {
        return std::nullopt;
      }
      GVariant* value = g_settings_get_user_value(s.settings.get(), "font-name");
      if (value == nullptr) {
        return std::nullopt;
      }
      std::string out = StringUtils::trim(g_variant_get_string(value, nullptr));
      g_variant_unref(value);
      if (out.empty()) {
        return std::nullopt;
      }
      return out;
    }

    float readTextScalingFactor() {
      const auto& s = interfaceSettings();
      if (s.settings == nullptr || !s.hasTextScalingFactor) {
        return 1.0F;
      }
      const auto factor = static_cast<float>(g_settings_get_double(s.settings.get(), "text-scaling-factor"));
      if (!std::isfinite(factor)) {
        return 1.0F;
      }
      return std::clamp(factor, kMinTextScalingFactor, kMaxTextScalingFactor);
    }

    std::optional<std::string> readSettingsIniFontName(const std::filesystem::path& path) {
      GKeyFile* keyFile = g_key_file_new();
      std::optional<std::string> out;
      if (g_key_file_load_from_file(keyFile, path.c_str(), G_KEY_FILE_NONE, nullptr) != FALSE) {
        if (gchar* raw = g_key_file_get_string(keyFile, "Settings", "gtk-font-name", nullptr); raw != nullptr) {
          std::string value = StringUtils::trim(raw);
          g_free(raw);
          if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
          }
          if (!value.empty()) {
            out = std::move(value);
          }
        }
      }
      g_key_file_free(keyFile);
      return out;
    }

    SystemFont parseFontName(const std::string& name) {
      SystemFont out;
      PangoFontDescription* desc = pango_font_description_from_string(name.c_str());
      if (desc == nullptr) {
        return out;
      }
      if (const char* family = pango_font_description_get_family(desc); family != nullptr) {
        out.family = StringUtils::trim(family);
      }
      const float size = static_cast<float>(pango_font_description_get_size(desc)) / static_cast<float>(PANGO_SCALE);
      if (std::isfinite(size) && size > 0.0F) {
        out.sizePx = pango_font_description_get_size_is_absolute(desc) != FALSE ? size : size * kPointsToPx;
      }
      pango_font_description_free(desc);
      return out;
    }

    std::vector<std::filesystem::path> settingsIniFiles() {
      const std::filesystem::path configDir = g_get_user_config_dir();
      return {configDir / "gtk-3.0" / "settings.ini", configDir / "gtk-4.0" / "settings.ini"};
    }

    SystemFont& storedSystemFont() {
      static SystemFont s;
      return s;
    }

  } // namespace

  float SystemFont::textScale(float referenceBodyPx) const noexcept {
    const float base = sizePx > 0.0F ? sizePx : referenceBodyPx;
    return base * textScalingFactor / referenceBodyPx;
  }

  std::vector<std::filesystem::path> systemFontWatchFiles() {
    std::vector<std::filesystem::path> candidates = settingsIniFiles();
    // dconf-service replaces this database on every GSettings write.
    candidates.push_back(std::filesystem::path(g_get_user_config_dir()) / "dconf" / "user");

    std::vector<std::filesystem::path> files;
    for (const auto& file : candidates) {
      std::error_code ec;
      if (std::filesystem::is_directory(file.parent_path(), ec)) {
        files.push_back(file);
      }
      // Dotfile managers symlink settings.ini; editors then write the target, not the link's directory.
      if (std::filesystem::is_symlink(file, ec)) {
        if (auto target = std::filesystem::canonical(file, ec); !ec) {
          files.push_back(std::move(target));
        }
      }
    }
    return files;
  }

  SystemFont readSystemFont() {
    std::optional<std::string> name = readGSettingsFontName();
    if (!name.has_value()) {
      for (const auto& path : settingsIniFiles()) {
        name = readSettingsIniFontName(path);
        if (name.has_value()) {
          break;
        }
      }
    }
    SystemFont out = name.has_value() ? parseFontName(*name) : SystemFont{};
    out.textScalingFactor = readTextScalingFactor();
    return out;
  }

  const SystemFont& currentSystemFont() noexcept { return storedSystemFont(); }

  bool refreshSystemFont() {
    SystemFont next = readSystemFont();
    if (next == storedSystemFont()) {
      return false;
    }
    storedSystemFont() = std::move(next);
    return true;
  }

  std::string resolveFontFamily(std::string_view configured) {
    std::string family = StringUtils::trim(configured);
    if (!family.empty()) {
      return family;
    }
    if (!storedSystemFont().family.empty()) {
      return storedSystemFont().family;
    }
    return "sans-serif";
  }

} // namespace text
