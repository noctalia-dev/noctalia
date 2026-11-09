#include "config/toml.h"
#include "render/core/color.h"
#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"
#include "system/process.h"
#include "tests/test_check.h"
#include "ui/palette.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

int main() {
  scripting::ScriptApiContext api;
  api.setConfigSnapshot(std::make_shared<const toml::table>(toml::parse("[shell]\noffline_mode = true")));
  api.setWallpaperPaths({{"DP-1", "/tmp/wallpaper.png"}});
  LuauHost host(api, "test/plugin:service");

  std::mutex mutex;
  std::condition_variable ready;
  std::optional<process::RunResult> result;
  int callbackRef = 0;
  host.setAsyncCommandResultHandler([&](std::uint64_t /*hostId*/, int ref, process::RunResult commandResult) {
    {
      std::scoped_lock lock(mutex);
      callbackRef = ref;
      result = std::move(commandResult);
    }
    ready.notify_one();
  });

  constexpr auto source = R"(
assert(not noctalia.runAsync(""))
assert(noctalia.runAsync(
  { "printf", "%s", "owner/repo; printf injected" },
  function(_) end,
  5000
))
assert(noctalia.getSetting("shell.offline_mode"))
assert(noctalia.wallpaperPath("DP-1") == "/tmp/wallpaper.png")
assert(noctalia.wallpaperPath("missing") == nil)
assert(type(noctalia.getColor("primary")) == "string")
assert(string.len(noctalia.getColor("primary")) == 7)
assert(string.sub(noctalia.getColor("primary"), 1, 1) == "#")
assert(noctalia.getColor("surface") ~= nil)
assert(noctalia.getColor("on_surface") ~= nil)
assert(noctalia.getColor("missing_role") == nil)
noctalia.setWallpaperMask("DP-1", {
  path = "/tmp/mask.png",
  wallpaperPath = "/tmp/wallpaper.png",
})
noctalia.setWallpaperMask("DP-1", nil)
)";
  TEST_CHECK(host.exec("=direct-argv", source));

  std::unique_lock lock(mutex);
  const bool completed = ready.wait_for(lock, std::chrono::seconds(10), [&] { return result.has_value(); });
  TEST_CHECK(completed);
  process::RunResult commandResult = std::move(*result);
  const int ref = callbackRef;
  lock.unlock();

  TEST_CHECK(commandResult.exitCode == 0);
  // shell metacharacters must remain inside one argv value
  TEST_CHECK(commandResult.out == "owner/repo; printf injected");
  TEST_CHECK(commandResult.err.empty());
  TEST_CHECK(host.callAsyncCommandCallback(ref, commandResult, std::chrono::milliseconds(50)));

  // getColor tracks active palette updates and returns nil for unknown roles
  const Palette originalPalette = palette;
  Palette testPalette = originalPalette;
  testPalette.primary = rgba(1.0F, 0.0F, 0.0F, 1.0F);
  setPalette(testPalette);
  TEST_CHECK(host.exec("=get-color", "assert(noctalia.getColor('primary') == '#FF0000')\n"));
  setPalette(originalPalette);
  TEST_CHECK(host.exec(
      "=get-color-restored",
      "assert(noctalia.getColor('primary') ~= '#FF0000')\n"
      "assert(noctalia.getColor('invalid_role') == nil)\n"
  ));

  return 0;
}
