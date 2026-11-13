// Locks how `noctalia msg config-set` splits its "<path> <value>" arguments. Both halves are TOML,
// so a quoted key segment, which a plugin id ("author/plugin") always needs, must reach the
// parser with its quotes, and the value must start right after the path.

#include "config/config_service.h"
#include "ipc/ipc_service.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-config-set-ipc-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", "[plugins]\nenabled = [\"me/demo\"]\n");
  writeFile(
      root / "data" / "noctalia" / "plugins" / "demo" / "plugin.toml",
      "id = \"me/demo\"\n"
      "name = \"Demo\"\n"
      "version = \"1.0.0\"\n"
      "plugin_api = 3\n"
      "[[setting]]\n"
      "key = \"enabled\"\n"
      "type = \"bool\"\n"
      "label_key = \"settings.enabled.label\"\n"
      "default = false\n"
      "[[setting]]\n"
      "key = \"label\"\n"
      "type = \"string\"\n"
      "label_key = \"settings.label.label\"\n"
      "default = \"\"\n"
      "[[widget]]\n"
      "id = \"main\"\n"
      "entry = \"main.luau\"\n"
  );
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);
  const std::filesystem::path overridesPath = root / "state" / "noctalia" / "settings.toml";

  {
    ConfigService config;
    IpcService ipc;
    config.registerIpc(ipc);

    // A bare dotted path keeps working.
    TEST_CHECK(ipc.execute("config-set shell.font_family 'Noto Sans'") == "ok\n");
    TEST_CHECK(config.config().shell.fontFamily == "Noto Sans");

    // A quoted segment keeps its quotes, and the value is everything after the path. Before the fix
    // the path was shell-unquoted and the value cut at the unquoted length, sending
    // `plugin_settings.me/demo.enabled=ed true` to the TOML parser.
    TEST_CHECK(ipc.execute(R"(config-set plugin_settings."me/demo".enabled true)") == "ok\n");
    TEST_CHECK(ipc.execute(R"(config-set plugin_settings.'me/demo'.label "two words")") == "ok\n");
    const std::string overrides = readFile(overridesPath);
    TEST_CHECK(overrides.contains(R"([plugin_settings."me/demo"])"));
    TEST_CHECK(overrides.contains("enabled = true"));
    TEST_CHECK(overrides.contains("label = 'two words'") || overrides.contains(R"(label = "two words")"));

    // Validation still applies to the plugin's declared settings.
    TEST_CHECK(ipc.execute(R"(config-set plugin_settings."me/demo".enabled 'yes')").starts_with("error: "));

    // A missing value or an unterminated quote is rejected, not guessed at.
    TEST_CHECK(ipc.execute("config-set shell.font_family").starts_with("error: expected arguments"));
    TEST_CHECK(
        ipc.execute(R"(config-set plugin_settings."me/demo.enabled true)").starts_with("error: expected arguments")
    );
  }

  std::filesystem::remove_all(root);
  return 0;
}
