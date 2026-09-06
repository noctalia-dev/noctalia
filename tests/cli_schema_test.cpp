#include "cli/schema_msg.h"
#include "tests/test_check.h"
#include "theme/builtin_palettes.h"
#include "theme/scheme.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace {

  const noctalia::cli::Command& child(const noctalia::cli::Command& command, std::string_view name) {
    const auto it = std::ranges::find(command.subcommands, name, &noctalia::cli::Command::name);
    TEST_CHECK(it != command.subcommands.end());
    return *it;
  }

} // namespace

int main() {
  const noctalia::cli::Command* colorSchemeSet = noctalia::cli::findMsgCommand("color-scheme-set");
  TEST_CHECK(colorSchemeSet != nullptr);

  const auto& builtin = child(*colorSchemeSet, "builtin");
  TEST_CHECK(builtin.positionals.size() == 1);
  const auto palettes = noctalia::theme::builtinPalettes();
  TEST_CHECK(builtin.positionals.front().choices.size() == palettes.size());
  for (std::size_t i = 0; i < palettes.size(); ++i)
    TEST_CHECK(builtin.positionals.front().choices[i] == palettes[i].name);

  const noctalia::cli::Command* barLayerSet = noctalia::cli::findMsgCommand("bar-layer-set");
  TEST_CHECK(barLayerSet != nullptr);
  TEST_CHECK(barLayerSet->positionals.size() == 3);
  const auto layerChoices = barLayerSet->positionals.front().choices;
  TEST_CHECK(layerChoices.size() == 3);
  TEST_CHECK(layerChoices[0] == "top");
  TEST_CHECK(layerChoices[1] == "overlay");
  TEST_CHECK(layerChoices[2] == "smart");

  const auto& wallpaper = child(*colorSchemeSet, "wallpaper");
  TEST_CHECK(wallpaper.positionals.size() == 1);
  TEST_CHECK(wallpaper.positionals.front().choices.size() == noctalia::theme::kSchemeNames.size());
  for (std::size_t i = 0; i < noctalia::theme::kSchemeNames.size(); ++i) {
    const std::string_view name = noctalia::theme::kSchemeNames[i];
    TEST_CHECK(wallpaper.positionals.front().choices[i] == name);
    const auto parsed = noctalia::theme::schemeFromString(name);
    TEST_CHECK(parsed.has_value());
    TEST_CHECK(noctalia::theme::schemeToString(*parsed) == name);
  }

  return EXIT_SUCCESS;
}
