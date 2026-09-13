// Quitting must not wait out a template hook that never exits. The worker is parked in a
// built-in undo hook that ignores SIGTERM, which is the hook path a shutdown request cannot
// skip, and destruction must still return within its grace bounds.

#include "config/config_service.h"
#include "tests/test_check.h"
#include "theme/palette.h"
#include "theme/template_apply_service.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

  using noctalia::theme::GeneratedPalette;
  using noctalia::theme::TemplateApplyService;

  GeneratedPalette darkPalette() {
    GeneratedPalette palette;
    palette.dark["mSurface"] = 0x111111;
    palette.light["mSurface"] = 0x111111;
    return palette;
  }

  void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-template-shutdown-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "noctalia");
  std::filesystem::create_directories(root / "state" / "noctalia");
  std::filesystem::create_directories(root / "data");
  std::filesystem::create_directories(root / "assets" / "templates");
  std::filesystem::create_directories(root / "assets" / "fonts");
  std::filesystem::create_directories(root / "assets" / "translations");

  const auto started = root / "started";
  // A minimal asset bundle: only the built-in template config is read by this test.
  writeFile(root / "assets" / "emoji.json", "{}\n");
  writeFile(root / "assets" / "fonts" / "noctalia-tabler.ttf", "");
  writeFile(root / "assets" / "translations" / "en.json", "{}\n");
  writeFile(
      root / "assets" / "templates" / "builtin.toml",
      "[templates.stuck]\nundo_hook = 'trap \"\" TERM; printf ran > " + started.string() + "; sleep 5'\n"
  );
  // The built-in template is no longer enabled, so applying owes its undo hook.
  writeFile(root / "config" / "noctalia" / "config.toml", "[theme.templates]\nenable_builtin_templates = false\n");
  writeFile(root / "state" / "noctalia" / "state.toml", "[theme_templates]\napplied_builtin_ids = 'stuck'\n");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
  ::setenv("NOCTALIA_ASSETS_DIR", (root / "assets").c_str(), 1);

  ConfigService config;
  auto service = std::make_unique<TemplateApplyService>(config, std::chrono::milliseconds(100));
  service->apply(darkPalette(), "dark", /*force=*/false, /*paletteChanged=*/true);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(started) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_CHECK(std::filesystem::exists(started));

  const auto start = std::chrono::steady_clock::now();
  service.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  TEST_CHECK(elapsed < std::chrono::seconds(2));

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  ::unsetenv("NOCTALIA_ASSETS_DIR");
  std::filesystem::remove_all(root);
  return 0;
}
