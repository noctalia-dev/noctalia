#include "hooks/hook_manager.h"
#include "tests/test_check.h"

#include <string>
#include <string_view>
#include <vector>

std::string_view hookKindKey(HookKind kind) {
  if (kind == HookKind::WallpaperChanged) {
    return "wallpaper_changed";
  }
  return "unknown";
}

int main() {
  constexpr const char* kPathName = "NOCTALIA_WALLPAPER_PATH";
  constexpr const char* kConnectorName = "NOCTALIA_WALLPAPER_CONNECTOR";

  HookManager hooks;
  HooksConfig config;
  config.commands[static_cast<std::size_t>(HookKind::WallpaperChanged)] = {"record-wallpaper-hook"};
  hooks.reload(config);

  std::vector<std::string> commands;
  std::vector<process::EnvOverride> envSeen;
  hooks.setCommandRunner([&](const std::string& command, const std::vector<process::EnvOverride>& env) {
    commands.push_back(command);
    envSeen = env;
    return true;
  });

  hooks.fire(HookKind::WallpaperChanged, {{kPathName, "/tmp/noctalia test/wallpaper.png"}, {kConnectorName, "DP-1"}});

  TEST_CHECK(commands.size() == 1);
  TEST_CHECK(commands[0] == "record-wallpaper-hook");
  TEST_CHECK(envSeen.size() == 2);
  TEST_CHECK(envSeen[0].name == kPathName && envSeen[0].value == "/tmp/noctalia test/wallpaper.png");
  TEST_CHECK(envSeen[1].name == kConnectorName && envSeen[1].value == "DP-1");

  return 0;
}
