#include "config/toml.h"
#include "render/core/color.h"
#include "scripting/luau_host.h"
#include "scripting/plugin_bindings.h"
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
  scripting::PluginBindingContext bindingContext;
  bindingContext.ownerId = "test/plugin:service";
  bindingContext.host = &host;
  host.setScriptContext(&bindingContext);

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

  api.setNightLightState({
      .enabled = true,
      .force = false,
      .active = true,
      .transitioning = true,
      .phase = "night",
      .currentTemperature = 4200,
      .targetTemperature = 4000,
      .dayTemperature = 6500,
      .nightTemperature = 4000,
  });
  TEST_CHECK(host.exec(
      "=nightlight-state",
      "local s = noctalia.nightlight.state()\n"
      "assert(s.enabled == true and s.force == false and s.active == true)\n"
      "assert(s.transitioning == true and s.phase == 'night')\n"
      "assert(s.currentTemperature == 4200 and s.targetTemperature == 4000)\n"
      "assert(s.dayTemperature == 6500 and s.nightTemperature == 4000)\n"
  ));

  bindingContext.beginCall({});
  TEST_CHECK(host.exec(
      "=nightlight-actions",
      "noctalia.nightlight.setEnabled(false)\n"
      "noctalia.nightlight.toggle()\n"
      "noctalia.nightlight.setForce(true)\n"
      "noctalia.nightlight.toggleForce()\n"
  ));
  TEST_CHECK(bindingContext.sideEffects.size() == 4);
  TEST_CHECK(
      bindingContext.sideEffects[0].kind == scripting::ScriptSideEffectKind::NightLightCommand
      && bindingContext.sideEffects[0].nightLightCommand == scripting::ScriptNightLightCommand::SetEnabled
      && !bindingContext.sideEffects[0].flag
  );
  TEST_CHECK(bindingContext.sideEffects[1].nightLightCommand == scripting::ScriptNightLightCommand::Toggle);
  TEST_CHECK(
      bindingContext.sideEffects[2].nightLightCommand == scripting::ScriptNightLightCommand::SetForce
      && bindingContext.sideEffects[2].flag
  );
  TEST_CHECK(bindingContext.sideEffects[3].nightLightCommand == scripting::ScriptNightLightCommand::ToggleForce);

  bindingContext.beginCall({});
  TEST_CHECK(host.exec(
      "=nightlight-watch",
      "watched = nil\n"
      "nightlightWatchId = noctalia.nightlight.watch(function(state) watched = state end)\n"
  ));
  TEST_CHECK(
      bindingContext.sideEffects.size() == 1
      && bindingContext.sideEffects[0].kind == scripting::ScriptSideEffectKind::NightLightWatch
  );
  const int watchId = bindingContext.sideEffects[0].callbackRef;
  TEST_CHECK(host.hasNightLightWatchCallback(watchId));
  TEST_CHECK(host.callNightLightWatchCallback(watchId, api.nightLightState(), std::chrono::milliseconds(50)));
  TEST_CHECK(host.exec(
      "=nightlight-watched", "assert(watched ~= nil and watched.enabled == true and watched.phase == 'night')\n"
  ));
  bindingContext.beginCall({});
  TEST_CHECK(host.exec("=nightlight-unwatch", "assert(noctalia.nightlight.unwatch(nightlightWatchId) == true)\n"));
  TEST_CHECK(!host.hasNightLightWatchCallback(watchId));

  return 0;
}
