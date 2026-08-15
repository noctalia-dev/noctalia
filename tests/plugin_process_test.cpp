#include "core/process/process.h"
#include "core/toml.h"
#include "scripting/luau_host.h"
#include "scripting/script_api_context.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <utility>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_process_test: {}", message);
    }
    return condition;
  }

} // namespace

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
noctalia.setWallpaperMask("DP-1", {
  path = "/tmp/mask.png",
  wallpaperPath = "/tmp/wallpaper.png",
})
noctalia.setWallpaperMask("DP-1", nil)
)";
  if (!expect(host.exec("=direct-argv", source), "argv call should be accepted")) {
    return 1;
  }

  std::unique_lock lock(mutex);
  const bool completed = ready.wait_for(lock, std::chrono::seconds(10), [&] { return result.has_value(); });
  if (!expect(completed, "argv process should complete")) {
    return 1;
  }
  process::RunResult commandResult = std::move(*result);
  const int ref = callbackRef;
  lock.unlock();

  bool ok = true;
  ok = expect(commandResult.exitCode == 0, "argv process should exit successfully") && ok;
  ok = expect(
           commandResult.out == "owner/repo; printf injected", "shell metacharacters must remain inside one argv value"
       )
      && ok;
  ok = expect(commandResult.err.empty(), "argv process should not write stderr") && ok;
  ok = expect(
           host.callAsyncCommandCallback(ref, commandResult, std::chrono::milliseconds(50)),
           "result callback should receive the completed process"
       )
      && ok;
  return ok ? 0 : 1;
}
