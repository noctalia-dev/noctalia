// Pins that bar-reserve-toggle persists reserve_space through ConfigService::setOverride,
// the same path Settings uses. IPC used to flip only BarInstance.barConfig, so attached
// panels (resolvePanelBarConfig reads config().bars) kept the old exclusive-zone inset.

#include "config/config_service.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "bar_reserve_toggle_persist_test: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(stderr, "bar_reserve_toggle_persist_test: missing bar.cpp path");
    return 1;
  }
  std::ifstream sourceFile(argv[1]);
  if (!sourceFile) {
    std::println(stderr, "bar_reserve_toggle_persist_test: cannot read {}", argv[1]);
    return 1;
  }
  std::ostringstream sourceStream;
  sourceStream << sourceFile.rdbuf();
  const std::string source = sourceStream.str();

  const auto togglePos = source.find("Bar::toggleBarReserveSpaceIpc");
  const auto nextFnPos = source.find("Bar::setBarAutoHideIpc");
  if (togglePos == std::string::npos || nextFnPos == std::string::npos || nextFnPos <= togglePos) {
    std::println(
        stderr, "bar_reserve_toggle_persist_test: toggleBarReserveSpaceIpc / setBarAutoHideIpc bounds not found"
    );
    return 1;
  }
  const std::string body = source.substr(togglePos, nextFnPos - togglePos);
  const bool persists = body.find("reserve_space") != std::string::npos
      && (body.find("setOverride") != std::string::npos || body.find("setOverrides") != std::string::npos);
  expect(persists, "toggleBarReserveSpaceIpc must persist reserve_space via setOverride/setOverrides");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-bar-reserve-toggle-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", "\n");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  {
    ConfigService config;
    expect(!config.config().bars.empty(), "default config has a bar");
    const std::string barName = config.config().bars.front().name;
    expect(config.config().bars.front().reserveSpace, "default reserveSpace is true");

    const std::vector<std::string> path{"bar", barName, "reserve_space"};
    expect(config.setOverride(path, false), "setOverride bar.reserve_space failed");
    expect(!config.config().bars.empty(), "bar still present after override");
    expect(config.config().bars.front().name == barName, "bar name unchanged");
    expect(!config.config().bars.front().reserveSpace, "config().bars reserveSpace did not follow setOverride");
  }

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("XDG_STATE_HOME");
  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::println("bar_reserve_toggle_persist_test passed");
    return 0;
  }
  std::println(stderr, "bar_reserve_toggle_persist_test: {} failure(s)", g_failures);
  return 1;
}
