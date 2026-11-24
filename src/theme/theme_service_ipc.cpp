#include "theme/theme_service.h"

#include "config/config_service.h"
#include "ipc/ipc_service.h"
#include "cli/schema_msg.h"
#include "util/string_utils.h"
#include "config/config_types.h"
#include <string_view>
#include <vector>

namespace nythera::theme {

  namespace {
    std::string colorSchemeValue(const ThemeConfig& theme) {
      switch (theme.source) {
      case PaletteSource::Builtin: return theme.builtinPalette;
      case PaletteSource::Wallpaper: return theme.wallpaperScheme;
      case PaletteSource::Community: return theme.communityPalette;
      case PaletteSource::Custom: return theme.customPalette;
      }
      return theme.builtinPalette;
    }

    std::string formatColorSchemeLine(const ThemeConfig& theme) {
      std::string out(enumToKey(kPaletteSources, theme.source));
      out.push_back(' ');
      out += colorSchemeValue(theme);
      out.push_back('\n');
      return out;
    }

    bool parseColorSchemeSetArgs(std::string_view args, PaletteSource& sourceOut, std::string& valueOut, std::string& errorOut) {
      const auto tokens = StringUtils::splitWhitespace(args);
      if (tokens.size() < 2) { errorOut = "error: expected <builtin|wallpaper|community|custom> <name-or-scheme>\n"; return false; }
      const auto source = enumFromKey(kPaletteSources, tokens.front());
      if (!source) { errorOut = "error: unknown palette source (expected builtin, wallpaper, community, or custom)\n"; return false; }
      std::string value;
      for (std::size_t i = 1; i < tokens.size(); ++i) { if (i > 1) value.push_back(' '); value += tokens[i]; }
      value = StringUtils::trim(value);
      if (value.empty()) { errorOut = "error: palette or scheme name required\n"; return false; }
      sourceOut = *source; valueOut = std::move(value); return true;
    }
  }

  void ThemeService::registerIpc(IpcService& ipc) {
    ipc.bind(nythera::cli::msg::themeModeToggle, [this](const std::string&) -> std::string {
      toggleLightDark();
      return "ok\n";
    });
    ipc.bind(
        nythera::cli::msg::themeModeGet,
        [this](const std::string&) -> std::string {
          std::string out(resolvedMode());
          out.push_back('\n');
          return out;
        },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(nythera::cli::msg::themeModeSet, [this](const std::string& args) -> std::string {
      const std::string token = StringUtils::trim(args);
      const auto mode = enumFromKey(kThemeModes, token);
      if (!mode.has_value()) {
        return "error: expected dark, light, or auto\n";
      }
      m_config.setThemeMode(*mode);
      return "ok\n";
    });
    ipc.bind(
        nythera::cli::msg::colorSchemeGet,
        [this](const std::string&) -> std::string { return formatColorSchemeLine(m_config.config().theme); },
        IpcService::HandlerOptions{.actionEditorVisibility = IpcService::ActionEditorVisibility::Hidden}
    );
    ipc.bind(nythera::cli::msg::colorSchemeSet, [this](const std::string& args) -> std::string {
      PaletteSource source = PaletteSource::Builtin;
      std::string value;
      std::string error;
      if (!parseColorSchemeSetArgs(args, source, value, error)) {
        return error;
      }
      if (!m_config.setThemeColorScheme(source, value)) {
        return "error: unknown scheme/palette or settings not writable\n";
      }
      return "ok\n";
    });
  }

} // namespace nythera::theme
