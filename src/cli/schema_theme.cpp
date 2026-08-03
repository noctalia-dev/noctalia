#include "cli/schema_theme.h"

#include <string_view>

namespace noctalia::theme {

  constexpr const char* kHelpText =
      "Usage: noctalia theme <image> [options]\n"
      "       noctalia theme --list-templates [-c <file>]\n"
      "\n"
      "Generate a color palette from an image. Material You and custom\n"
      "schemes produce very different results.\n"
      "\n"
      "Options:\n"
      "  --scheme <name>   Material You (Material Design 3):\n"
      "                      m3-tonal-spot  (default)\n"
      "                      m3-content\n"
      "                      m3-fruit-salad\n"
      "                      m3-rainbow\n"
      "                      m3-monochrome\n"
      "                    Custom (HSL-space, non-M3):\n"
      "                      vibrant\n"
      "                      faithful\n"
      "                      soft\n"
      "                      dysfunctional\n"
      "                      muted\n"
      "  --dark            Emit only the dark variant (default)\n"
      "  --light           Emit only the light variant\n"
      "  --both            Emit both variants under dark/light keys\n"
      "  --pure-black      Re-anchor the dark surface ramp to true black (OLED)\n"
      "  --theme-json <f>  Load precomputed dark/light token maps from JSON\n"
      "  -o <file>         Write JSON to file instead of stdout\n"
      "  -r <in:out>       Render a template file to an output path\n"
      "  -c <file>         Process a TOML template config file\n"
      "  --builtin-config  Process the shipped built-in template catalog\n"
      "  --list-templates  List built-in, cached community, and configured user templates\n"
      "                    Use -c <file> to include a specific template config\n"
      "  --default-mode    Template default mode: dark or light";

  constexpr std::string_view kThemeSchemes[] = {"m3-tonal-spot", "m3-content", "m3-fruit-salad", "m3-rainbow",
                                                "m3-monochrome", "vibrant",    "faithful",       "soft",
                                                "dysfunctional", "muted"};

  constexpr std::string_view kDefaultModes[] = {"dark", "light"};

  constexpr cli_schema::CliPositional kThemePositionals[] = {
      {.name = "image", .description = "path to source image file", .required = false}
  };

  constexpr cli_schema::CliFlag kThemeFlags[] = {
      {.longName = "--scheme", .description = "Material You or HSL scheme name", .takesValue = true},
      {.longName = "--dark", .description = "Emit only the dark variant"},
      {.longName = "--light", .description = "Emit only the light variant"},
      {.longName = "--both", .description = "Emit both variants under dark/light keys"},
      {.longName = "--pure-black", .description = "Re-anchor the dark surface ramp to true black"},
      {.longName = "--theme-json",
       .description = "Load precomputed dark/light token maps from JSON",
       .takesValue = true},
      {.longName = "-o", .description = "Write JSON to file instead of stdout", .takesValue = true},
      {.longName = "-r",
       .shortName = "--render",
       .description = "Render a template file to an output path (in:out)",
       .takesValue = true},
      {.longName = "-c",
       .shortName = "--config",
       .description = "Process a TOML template config file",
       .takesValue = true},
      {.longName = "--builtin-config", .description = "Process the shipped built-in template catalog"},
      {.longName = "--list-templates", .description = "List built-in, cached community, and user templates"},
      {.longName = "--default-mode", .description = "Template default mode: dark or light", .takesValue = true},
  };

  extern constexpr cli_schema::CliCommand kThemeCmd = {
      .name = "theme",
      .summary = "Generate a color palette from an image",
      .helpText = kHelpText,
      .flags = kThemeFlags,
      .positionals = kThemePositionals,
      .extraArgError = "error: usage: noctalia theme <image> [options]"
  };

} // namespace noctalia::theme
