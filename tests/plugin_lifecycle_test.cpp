#include "config/config_service.h"
#include "config/toml.h"
#include "core/deferred_call.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_script_watcher.h"
#include "scripting/plugin_service_host.h"
#include "scripting/plugin_state_store.h"
#include "scripting/script_api_context.h"
#include "scripting/script_runtime.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  std::filesystem::path gRoot;

  void removeRoot() { std::filesystem::remove_all(gRoot); }

  bool waitForState(std::string_view pluginId, std::string_view key) {
    constexpr auto timeout = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (scripting::PluginStateStore::instance().get(std::string(pluginId), std::string(key)).has_value()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  bool drainUntil(const std::function<bool()>& predicate) {
    constexpr auto timeout = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (auto& callback : DeferredCall::takePending()) {
        callback();
      }
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  bool writeText(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    auto* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const bool written = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    return std::fclose(file) == 0 && written;
  }

} // namespace

int main() {
  scripting::ScriptApiContext api;

  {
    scripting::ScriptRuntime runtime("test/enable:service", {}, api, {});
    runtime.start(
        "=enable",
        "noctalia.state.set('loaded', true)\n"
        "function onEnable()\n"
        "  noctalia.state.set('enabled', true)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/enable", "loaded"));
    TEST_CHECK(!scripting::PluginStateStore::instance().get("test/enable", "enabled").has_value());
    TEST_CHECK(runtime.enqueueCall("onEnable", {}));
    TEST_CHECK(waitForState("test/enable", "enabled"));
  }

  {
    scripting::ScriptRuntime runtime("test/deactivate:service", {}, api, {});
    runtime.start(
        "=deactivate",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(signal, reason)\n"
        "  noctalia.state.set('exit_signal', signal)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/deactivate", "loaded"));
    runtime.stop(scripting::ScriptExitReason::Disable);
  }

  TEST_CHECK(scripting::PluginStateStore::instance().get("test/deactivate", "exit_signal") == "0");
  TEST_CHECK(scripting::PluginStateStore::instance().get("test/deactivate", "exit_reason") == R"("disable")");

  {
    scripting::ScriptRuntime runtime("test/remove:service", {}, api, {});
    runtime.start(
        "=remove",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(signal, reason)\n"
        "  noctalia.state.set('exit_signal', signal)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/remove", "loaded"));
    runtime.stop(scripting::ScriptExitReason::Uninstall);
  }

  TEST_CHECK(scripting::PluginStateStore::instance().get("test/remove", "exit_signal") == "0");
  TEST_CHECK(scripting::PluginStateStore::instance().get("test/remove", "exit_reason") == R"("uninstall")");

  {
    scripting::ScriptRuntime runtime("test/ordinary-stop:service", {}, api, {});
    runtime.start(
        "=ordinary-stop",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(signal, reason)\n"
        "  noctalia.state.set('exit_signal', signal)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/ordinary-stop", "loaded"));
    runtime.stop();
  }

  TEST_CHECK(
      scripting::PluginStateStore::instance().get("test/ordinary-stop", "exit_signal") == "0"
      && scripting::PluginStateStore::instance().get("test/ordinary-stop", "exit_reason") == R"("reload")"
  );

  {
    scripting::PluginExitReasonScope scope("test/non-service", scripting::ScriptExitReason::Disable);
    scripting::ScriptRuntime runtime("test/non-service:widget", {}, api, {});
    runtime.start(
        "=non-service-disable",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(_signal, reason)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/non-service", "loaded"));
    runtime.stop();
  }

  TEST_CHECK(scripting::PluginStateStore::instance().get("test/non-service", "exit_reason") == R"("disable")");

  {
    scripting::ScriptRuntime runtime("test/reload:service", {}, api, {});
    runtime.start(
        "=before-reload",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(signal, reason)\n"
        "  noctalia.state.set('exit_signal', signal)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/reload", "loaded"));
    runtime.reload("=after-reload", "noctalia.state.set('reloaded', true)\n", {});
    TEST_CHECK(waitForState("test/reload", "reloaded"));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/reload", "exit_signal") == "0"
        && scripting::PluginStateStore::instance().get("test/reload", "exit_reason") == R"("reload")"
    );
  }

  {
    // A shell config value read via getSetting reloads a runtime without onConfigChanged
    // when it changes, and only then.
    const auto snapshot = [](std::string_view toml) { return std::make_shared<const toml::table>(toml::parse(toml)); };
    api.setConfigSnapshot(snapshot("[shell]\noffline_mode = true\n[bar]\nposition = 'top'"));
    scripting::ScriptRuntime runtime("test/get-setting-reload:service", {}, api, {});
    runtime.start(
        "=get-setting-reload",
        "local loads = (noctalia.state.get('loads') or 0) + 1\n"
        "noctalia.state.set('loads', loads)\n"
        "noctalia.state.set('offline_' .. tostring(noctalia.getSetting('shell.offline_mode')), loads)\n",
        {}
    );
    TEST_CHECK(waitForState("test/get-setting-reload", "offline_true"));
    api.setConfigSnapshot(snapshot("[shell]\noffline_mode = true\n[bar]\nposition = 'bottom'"));
    api.setConfigSnapshot(snapshot("[shell]\noffline_mode = false\n[bar]\nposition = 'bottom'"));
    TEST_CHECK(waitForState("test/get-setting-reload", "offline_false"));
    TEST_CHECK(scripting::PluginStateStore::instance().get("test/get-setting-reload", "offline_false") == "2");
  }

  {
    // With onConfigChanged defined, the handler runs in place instead of a reload.
    const auto snapshot = [](std::string_view toml) { return std::make_shared<const toml::table>(toml::parse(toml)); };
    api.setConfigSnapshot(snapshot("[shell]\noffline_mode = true"));
    scripting::ScriptRuntime runtime("test/get-setting-handler:service", {}, api, {});
    runtime.start(
        "=get-setting-handler",
        "local loads = (noctalia.state.get('loads') or 0) + 1\n"
        "noctalia.state.set('loads', loads)\n"
        "noctalia.getSetting('shell.offline_mode')\n"
        "function onConfigChanged()\n"
        "  noctalia.state.set('changed', noctalia.getSetting('shell.offline_mode'))\n"
        "end\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return runtime.hasOnConfigChanged(); }));
    api.setConfigSnapshot(snapshot("[shell]\noffline_mode = false"));
    TEST_CHECK(waitForState("test/get-setting-handler", "changed"));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/get-setting-handler", "changed") == "false"
        && scripting::PluginStateStore::instance().get("test/get-setting-handler", "loads") == "1"
    );
  }

  {
    scripting::ScriptRuntime runtime("test/shutdown:service", {}, api, {});
    runtime.start(
        "=shutdown",
        "noctalia.state.set('loaded', true)\n"
        "function onExit(signal, reason)\n"
        "  noctalia.state.set('exit_signal', signal)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/shutdown", "loaded"));
    scripting::ScriptRuntime::setShutdownSignal(15);
    runtime.stop();
    scripting::ScriptRuntime::setShutdownSignal(0);
  }

  TEST_CHECK(
      scripting::PluginStateStore::instance().get("test/shutdown", "exit_signal") == "15"
      && scripting::PluginStateStore::instance().get("test/shutdown", "exit_reason") == R"("shutdown")"
  );

  gRoot = std::filesystem::temp_directory_path() / ("noctalia-plugin-lifecycle-test-" + std::to_string(::getpid()));
  const auto& root = gRoot;
  std::filesystem::remove_all(root);
  std::atexit(removeRoot);
  std::filesystem::create_directories(root / "config");
  std::filesystem::create_directories(root / "state");
  std::filesystem::create_directories(root / "data");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
  const auto modulePluginDir = root / "module-plugin";
  TEST_CHECK(
      writeText(modulePluginDir / "main.luau", "return true\n")
      && writeText(modulePluginDir / "modules/value.luau", "return 42\n")
      && writeText(modulePluginDir / "sibling.luau", "return 'plugin-root'\n")
      && writeText(modulePluginDir / "modules/sibling.luau", "return 'module-dir'\n")
      && writeText(
          modulePluginDir / "modules/counter.luau",
          "moduleRuns = (moduleRuns or 0) + 1\n"
          "local value = require('./value.luau')\n"
          "local M = { runs = moduleRuns, value = value, ownG = _G }\n"
          "function M.deferredSibling()\n"
          "  return require('./sibling.luau')\n"
          "end\n"
          "return M\n"
      )
      && writeText(modulePluginDir / "modules/lazy.luau", "return 'lazy'\n")
      && writeText(modulePluginDir / "modules/cycle_a.luau", "return require('./cycle_b.luau')\n")
      && writeText(modulePluginDir / "modules/cycle_b.luau", "return require('./cycle_a.luau')\n")
      // Mentions _G: its _G is the writable module env, so the env stays unsafe
      // and a builtin shadowed through _G must stay visible to global reads.
      && writeText(
          modulePluginDir / "modules/gated.luau",
          "local realPairs = pairs\n"
          "_G.pairs = function(t)\n"
          "  return realPairs(t)\n"
          "end\n"
          // Read through the global before touching getfenv: that call clears the
          // env's safe flag, which would mask a stale cached read.
          "local shadowVisible = pairs == _G.pairs\n"
          "local sameEnv = _G == getfenv(1)\n"
          "local M = { sameEnv = sameEnv, shadowVisible = shadowVisible }\n"
          "return M\n"
      )
      // No _G token anywhere: the env is cacheable, so the loader must install it as
      // the thread globals for the load and mark it safe to keep Luau's fast paths.
      // SAMPLE_GAP contains _G without naming it, so a substring match instead of a
      // token match would drop this module onto the slow path and the timing below
      // would catch it.
      && writeText(
          modulePluginDir / "modules/ungated.luau",
          "local M = {}\n"
          "local SAMPLE_GAP = 4\n"
          "local buf = buffer.create(65536)\n"
          "function M.pass()\n"
          "  local o = 0\n"
          "  local acc = 0.0\n"
          "  while o < 65536 do\n"
          "    buffer.writef32(buf, o, buffer.readf32(buf, o) * 1.0000001)\n"
          "    buffer.writef32(buf, o + 4, buffer.readf32(buf, o + 4) * 1.0000001)\n"
          "    buffer.writef32(buf, o + 8, buffer.readf32(buf, o + 8) * 1.0000001)\n"
          "    buffer.writef32(buf, o + 12, buffer.readf32(buf, o + 12) * 1.0000001)\n"
          "    o += SAMPLE_GAP + 12\n"
          "  end\n"
          "  return acc\n"
          "end\n"
          "return M\n"
      )
  );

  // The runtime reports canonical paths; compare against the same form so a
  // symlinked temp dir does not fail the expectations below.
  std::error_code moduleDirEc;
  const auto canonicalModuleDir = std::filesystem::canonical(modulePluginDir, moduleDirEc);
  TEST_CHECK(!moduleDirEc);

  {
    std::vector<std::filesystem::path> modulePaths;
    scripting::ScriptRuntime runtime("test/require:service", {}, api, modulePluginDir);
    const auto subscription = runtime.subscribe([&](const scripting::ScriptResult& result) {
      if (result.modulePathsKnown) {
        modulePaths = result.modulePaths;
      }
    });
    runtime.start(
        (modulePluginDir / "main.luau").string(),
        "local first = require('./modules/counter.luau')\n"
        // Same file by a non-normalized spelling: one cache slot, one dependency.
        "local second = require('./modules/../modules/counter.luau')\n"
        "noctalia.state.set('value', first.value)\n"
        "noctalia.state.set('cached', first == second and first.runs == 1)\n"
        "noctalia.state.set('globals_isolated', moduleGlobal == nil and moduleRuns == nil)\n"
        "noctalia.state.set('own_globals', first.ownG ~= _G)\n"
        // Resolves against the module's own directory even though it runs from here.
        "noctalia.state.set('deferred_base', first.deferredSibling())\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return waitForState("test/require", "deferred_base") && modulePaths.size() == 3; }));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/require", "value") == "42"
        && scripting::PluginStateStore::instance().get("test/require", "cached") == "true"
    );
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/require", "globals_isolated") == "true"
        && scripting::PluginStateStore::instance().get("test/require", "own_globals") == "true"
    );
    TEST_CHECK(scripting::PluginStateStore::instance().get("test/require", "deferred_base") == R"("module-dir")");
    TEST_CHECK(
        std::ranges::contains(modulePaths, canonicalModuleDir / "modules/counter.luau")
        && std::ranges::contains(modulePaths, canonicalModuleDir / "modules/value.luau")
        && std::ranges::contains(modulePaths, canonicalModuleDir / "modules/sibling.luau")
    );
    runtime.unsubscribe(subscription);
  }

  {
    // A module first required inside a callback still joins the watched set.
    std::vector<std::filesystem::path> modulePaths;
    bool publishedEmptySet = false;
    scripting::ScriptRuntime runtime("test/require-lazy:service", {}, api, modulePluginDir);
    const auto subscription = runtime.subscribe([&](const scripting::ScriptResult& result) {
      if (result.modulePathsKnown) {
        publishedEmptySet = publishedEmptySet || result.modulePaths.empty();
        modulePaths = result.modulePaths;
      }
    });
    runtime.start(
        (modulePluginDir / "lazy-main.luau").string(),
        "function onIpc()\n"
        "  noctalia.state.set('lazy', require('./modules/lazy.luau'))\n"
        "end\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return publishedEmptySet; }));
    (void)runtime.enqueueCall("onIpc", {});
    TEST_CHECK(drainUntil([&] {
      return modulePaths.size() == 1 && modulePaths.front() == canonicalModuleDir / "modules/lazy.luau";
    }));
    runtime.unsubscribe(subscription);
  }

  {
    // A require that fails must not be cached, reported, or watched.
    std::vector<std::filesystem::path> modulePaths;
    bool sawResult = false;
    scripting::ScriptRuntime runtime("test/require-missing:service", {}, api, modulePluginDir);
    const auto subscription = runtime.subscribe([&](const scripting::ScriptResult& result) {
      sawResult = true;
      if (result.modulePathsKnown) {
        modulePaths = result.modulePaths;
      }
    });
    runtime.start(
        (modulePluginDir / "missing-main.luau").string(),
        "local ok, err = pcall(function() require('./modules/absent.luau') end)\n"
        "noctalia.state.set('missing_failed', ok == false)\n"
        "noctalia.state.set('missing_message', string.find(err, 'cannot open') ~= nil)\n"
        "local ok2, err2 = pcall(function() require('modules/value.luau') end)\n"
        "noctalia.state.set('shape_message', string.find(err2, 'must be relative') ~= nil)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return sawResult && waitForState("test/require-missing", "shape_message"); }));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/require-missing", "missing_failed") == "true"
        && scripting::PluginStateStore::instance().get("test/require-missing", "missing_message") == "true"
        && scripting::PluginStateStore::instance().get("test/require-missing", "shape_message") == "true"
    );
    TEST_CHECK(modulePaths.empty());
    runtime.unsubscribe(subscription);
  }

  {
    std::string cycleError;
    scripting::ScriptRuntime runtime("test/require-cycle:service", {}, api, modulePluginDir);
    const auto subscription = runtime.subscribe([&](const scripting::ScriptResult& result) {
      if (!result.ok) {
        cycleError = result.error;
      }
    });
    runtime.start((modulePluginDir / "cycle-main.luau").string(), "require('./modules/cycle_a.luau')\n", {});
    TEST_CHECK(drainUntil([&] { return cycleError.contains("circular require"); }));
    TEST_CHECK(cycleError.contains("cycle_a.luau") && cycleError.contains("cycle_b.luau"));
    runtime.unsubscribe(subscription);
  }

  {
    // A module keeps Luau's fast global paths. The loader installs the module env as the
    // thread globals for the load and marks it safe, so the chunk resolves global reads and
    // builtin calls from the load-time cache instead of a metatable lookup per access. The
    // same loop is timed in the entry chunk and in the module and the ratio decides, since
    // both paths compute the same thing: without the fast path a module loop runs several
    // times slower than the entry chunk (about 7x in this host) and at the same speed with
    // it (about 1.0x). Scheduler noise inflates both sides, so each side takes its best of
    // three rounds.
    scripting::ScriptRuntime runtime("test/require-speed:service", {}, api, modulePluginDir);
    runtime.start(
        (modulePluginDir / "speed-main.luau").string(),
        "local gated = require('./modules/gated.luau')\n"
        "local fast = require('./modules/ungated.luau')\n"
        "noctalia.state.set('module_G_is_env', gated.sameEnv)\n"
        "noctalia.state.set('module_G_shadow_visible', gated.shadowVisible)\n"
        "local buf = buffer.create(65536)\n"
        "local function pass()\n"
        "  local o = 0\n"
        "  local acc = 0.0\n"
        "  while o < 65536 do\n"
        "    buffer.writef32(buf, o, buffer.readf32(buf, o) * 1.0000001)\n"
        "    buffer.writef32(buf, o + 4, buffer.readf32(buf, o + 4) * 1.0000001)\n"
        "    buffer.writef32(buf, o + 8, buffer.readf32(buf, o + 8) * 1.0000001)\n"
        "    buffer.writef32(buf, o + 12, buffer.readf32(buf, o + 12) * 1.0000001)\n"
        "    o += 16\n"
        "  end\n"
        "  return acc\n"
        "end\n"
        // Size the work against this machine so the chunk stays well inside its CPU budget
        // and a module on the slow path still finishes the script: the ratio, not the
        // absolute time, is what the test checks.
        "local calibrateAt = os.clock()\n"
        "pass()\n"
        "local perPass = math.max(os.clock() - calibrateAt, 1e-9)\n"
        "local passes = math.clamp(math.floor(0.004 / perPass), 1, 200)\n"
        "local entryBest, moduleBest = 1e9, 1e9\n"
        "local agree = true\n"
        "for _ = 1, 3 do\n"
        "  local entryAt = os.clock()\n"
        "  local entry = 0.0\n"
        "  for _ = 1, passes do entry += pass() end\n"
        "  entryBest = math.min(entryBest, os.clock() - entryAt)\n"
        "  local moduleAt = os.clock()\n"
        "  local module = 0.0\n"
        "  for _ = 1, passes do module += fast.pass() end\n"
        "  moduleBest = math.min(moduleBest, os.clock() - moduleAt)\n"
        "  agree = agree and entry == module\n"
        "end\n"
        "noctalia.state.set('speed_loops_agree', agree)\n"
        "noctalia.state.set('speed_ratio', moduleBest / entryBest)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return waitForState("test/require-speed", "speed_ratio"); }));
    const auto speedState = [](std::string_view key) {
      return scripting::PluginStateStore::instance().get("test/require-speed", std::string(key));
    };
    TEST_CHECK(speedState("module_G_is_env") == "true");
    TEST_CHECK(speedState("module_G_shadow_visible") == "true");
    TEST_CHECK(speedState("speed_loops_agree") == "true");
    double ratio = 0.0;
    if (const auto raw = speedState("speed_ratio"); raw.has_value()) {
      try {
        ratio = std::stod(*raw);
      } catch (const std::exception&) {
        ratio = 0.0;
      }
    }
    // A ratio out of range means a required module paid a metatable lookup per global read.
    TEST_CHECK(ratio > 0.0 && ratio < 2.5);
  }

  {
    FileWatcher watcher;
    scripting::PluginScriptWatcher scriptWatcher;
    int reloads = 0;
    scriptWatcher.start(&watcher, modulePluginDir / "main.luau", [&] { ++reloads; });
    const std::vector<std::filesystem::path> modulePaths{modulePluginDir / "modules/value.luau"};
    scriptWatcher.setModulePaths(modulePaths);
    TEST_CHECK(writeText(modulePaths.front(), "return 43\n"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (reloads == 0 && std::chrono::steady_clock::now() < deadline) {
      watcher.dispatch();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    TEST_CHECK(reloads == 1);
  }

  struct SoundLoadRequest {
    std::uint64_t ownerId = 0;
    std::string name;
    std::string path;
  };
  struct SoundPlayRequest {
    std::uint64_t ownerId = 0;
    std::string name;
  };

  std::vector<SoundLoadRequest> soundLoads;
  std::vector<SoundPlayRequest> soundPlays;
  std::vector<std::uint64_t> unloadedSoundOwners;
  std::function<void()> onSoundLoad;
  api.setLoadSoundHook(
      [&](std::uint64_t ownerId, const std::string& name, const std::string& path) -> std::optional<std::string> {
        soundLoads.push_back({.ownerId = ownerId, .name = name, .path = path});
        if (name == "stale" && onSoundLoad) {
          auto callback = std::move(onSoundLoad);
          onSoundLoad = {};
          callback();
        }
        if (name == "broken") {
          return "decode failed";
        }
        return std::nullopt;
      }
  );
  api.setPlaySoundHook([&](std::uint64_t ownerId, const std::string& name) {
    soundPlays.push_back({.ownerId = ownerId, .name = name});
  });
  api.setUnloadPluginSoundsHook([&](std::uint64_t ownerId) { unloadedSoundOwners.push_back(ownerId); });

  soundLoads.clear();
  soundPlays.clear();
  unloadedSoundOwners.clear();
  const auto soundPluginDir = root / "sound-plugin";
  {
    scripting::ScriptRuntime runtime("test/sound-outcomes:service", {}, api, soundPluginDir);
    runtime.start(
        "=sound-outcomes",
        "local successAccepted = noctalia.sound.load('click', 'sounds/click.wav', function(ok, err)\n"
        "  noctalia.state.set('success_ok', ok)\n"
        "  noctalia.state.set('success_error_nil', err == nil)\n"
        "  if ok then noctalia.sound.play('click') end\n"
        "end)\n"
        "noctalia.state.set('success_accepted', successAccepted)\n"
        "local failureAccepted = noctalia.sound.load('broken', 'sounds/broken.wav', function(ok, err)\n"
        "  noctalia.state.set('failure_ok', ok)\n"
        "  noctalia.state.set('failure_error', err)\n"
        "  if ok then noctalia.sound.play('broken') end\n"
        "end)\n"
        "noctalia.state.set('failure_accepted', failureAccepted)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] {
      return scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_error_nil").has_value()
          && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_error").has_value()
          && soundPlays.size() == 1;
    }));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_accepted") == "true"
        && scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_ok") == "true"
        && scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_error_nil") == "true"
    );
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_accepted") == "true"
        && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_ok") == "false"
        && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_error") == R"("decode failed")"
    );
    const auto clickLoad =
        std::ranges::find_if(soundLoads, [](const SoundLoadRequest& request) { return request.name == "click"; });
    TEST_CHECK(clickLoad != soundLoads.end() && clickLoad->path == (soundPluginDir / "sounds/click.wav").string());
    TEST_CHECK(
        soundPlays.size() == 1
        && clickLoad != soundLoads.end()
        && soundPlays.front().ownerId == clickLoad->ownerId
        && soundPlays.front().name == "click"
    );
  }
  TEST_CHECK(drainUntil([&] { return unloadedSoundOwners.size() == 1; }));

  soundLoads.clear();
  soundPlays.clear();
  unloadedSoundOwners.clear();
  {
    scripting::ScriptRuntime runtime("test/sound-limits:service", {}, api, soundPluginDir);
    runtime.start(
        "=sound-limits",
        "local accepted = 0\n"
        "for index = 1, 8 do\n"
        "  if noctalia.sound.load('pending' .. index, 'sound.wav', function() end) then\n"
        "    accepted = accepted + 1\n"
        "  end\n"
        "end\n"
        "local duplicate = noctalia.sound.load('pending1', 'other.wav', function() end)\n"
        "local ninth = noctalia.sound.load('pending9', 'sound.wav', function() end)\n"
        "noctalia.state.set('accepted_count', accepted)\n"
        "noctalia.state.set('duplicate_accepted', duplicate)\n"
        "noctalia.state.set('ninth_accepted', ninth)\n",
        {}
    );
    TEST_CHECK(waitForState("test/sound-limits", "ninth_accepted"));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/sound-limits", "accepted_count") == "8"
        && scripting::PluginStateStore::instance().get("test/sound-limits", "duplicate_accepted") == "false"
        && scripting::PluginStateStore::instance().get("test/sound-limits", "ninth_accepted") == "false"
    );
    TEST_CHECK(drainUntil([&] { return soundLoads.size() == 8; }));
  }
  TEST_CHECK(drainUntil([&] { return unloadedSoundOwners.size() == 1; }));

  soundLoads.clear();
  unloadedSoundOwners.clear();
  {
    scripting::ScriptRuntime runtime("test/sound-unhealthy:service", {}, api, soundPluginDir);
    runtime.start(
        "=sound-unhealthy",
        "for index = 1, 5 do\n"
        "  noctalia.sound.load('failure' .. index, 'sound.wav', function()\n"
        "    error('deliberate callback failure')\n"
        "  end)\n"
        "end\n"
        "local accepted = noctalia.sound.load('after-errors', 'sound.wav', function(ok, err)\n"
        "  noctalia.state.set('completion_after_unhealthy', ok and err == nil)\n"
        "end)\n"
        "noctalia.state.set('after_errors_accepted', accepted)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] {
      return runtime.unhealthy()
          && scripting::PluginStateStore::instance()
                 .get("test/sound-unhealthy", "completion_after_unhealthy")
                 .has_value();
    }));
    TEST_CHECK(
        scripting::PluginStateStore::instance().get("test/sound-unhealthy", "after_errors_accepted") == "true"
        && scripting::PluginStateStore::instance().get("test/sound-unhealthy", "completion_after_unhealthy") == "true"
    );
  }
  TEST_CHECK(drainUntil([&] { return unloadedSoundOwners.size() == 1; }));

  soundLoads.clear();
  soundPlays.clear();
  unloadedSoundOwners.clear();
  std::uint64_t firstOwner = 0;
  std::uint64_t secondOwner = 0;
  {
    scripting::ScriptRuntime first("test/sound-isolation:service", {}, api, root / "sound-first");
    scripting::ScriptRuntime second("test/sound-isolation:service", {}, api, root / "sound-second");
    const std::string source = "noctalia.sound.load('click', 'click.wav', function(ok)\n"
                               "  if ok then noctalia.sound.play('click') end\n"
                               "end)\n";
    first.start("=sound-first", source, {});
    second.start("=sound-second", source, {});
    TEST_CHECK(drainUntil([&] { return soundLoads.size() == 2 && soundPlays.size() == 2; }));
    firstOwner = soundLoads[0].ownerId;
    secondOwner = soundLoads[1].ownerId;
    TEST_CHECK(firstOwner != 0 && secondOwner != 0 && firstOwner != secondOwner);
    TEST_CHECK(
        std::ranges::count_if(
            soundPlays,
            [firstOwner](const SoundPlayRequest& request) {
              return request.ownerId == firstOwner && request.name == "click";
            }
        ) == 1
        && std::ranges::count_if(
               soundPlays, [secondOwner](const SoundPlayRequest& request) {
                 return request.ownerId == secondOwner && request.name == "click";
               }
           ) == 1
    );
  }
  TEST_CHECK(drainUntil([&] {
    return std::ranges::count(unloadedSoundOwners, firstOwner) == 1
        && std::ranges::count(unloadedSoundOwners, secondOwner) == 1;
  }));

  soundLoads.clear();
  unloadedSoundOwners.clear();
  std::uint64_t staleOwner = 0;
  {
    scripting::ScriptRuntime runtime("test/sound-stale:service", {}, api, root / "sound-stale");
    onSoundLoad = [&runtime] {
      runtime.reload(
          "=sound-replacement",
          "noctalia.state.set('replacement_loaded', true)\n"
          "noctalia.state.set('stale_callback_reached', false)\n",
          {}
      );
    };
    runtime.start(
        "=sound-stale",
        "noctalia.sound.load('stale', 'stale.wav', function()\n"
        "  noctalia.state.set('stale_callback_reached', true)\n"
        "end)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] {
      if (!soundLoads.empty()) {
        staleOwner = soundLoads.front().ownerId;
      }
      return staleOwner != 0
          && scripting::PluginStateStore::instance().get("test/sound-stale", "replacement_loaded").has_value()
          && std::ranges::count(unloadedSoundOwners, staleOwner) == 1;
    }));
    TEST_CHECK(scripting::PluginStateStore::instance().get("test/sound-stale", "stale_callback_reached") == "false");
  }
  drainUntil([&] { return unloadedSoundOwners.size() >= 2; });
  TEST_CHECK(writeText(
      root / "path-plugins/plugin/plugin.toml",
      "id = \"test/plugin\"\n"
      "name = \"Lifecycle test\"\n"
      "version = \"1.0.0\"\n"
      "plugin_api = 17\n"
  ));
  TEST_CHECK(
      writeText(
          root / "path-plugins/api16/plugin.toml",
          "id = \"test/api16\"\n"
          "name = \"API 16 service\"\n"
          "version = \"1.0.0\"\n"
          "plugin_api = 16\n"
          "[[service]]\n"
          "id = \"service\"\n"
          "entry = \"service.luau\"\n"
      )
      && writeText(
          root / "path-plugins/api16/service.luau",
          "noctalia.state.set('loaded', true)\n"
          "function onEnable()\n"
          "  noctalia.state.set('enabled', true)\n"
          "end\n"
      )
      && writeText(
          root / "path-plugins/api17/plugin.toml",
          "id = \"test/api17\"\n"
          "name = \"API 17 service\"\n"
          "version = \"1.0.0\"\n"
          "plugin_api = 17\n"
          "[[service]]\n"
          "id = \"service\"\n"
          "entry = \"service.luau\"\n"
      )
      && writeText(
          root / "path-plugins/api17/service.luau",
          "noctalia.state.set('loaded', true)\n"
          "function onEnable()\n"
          "  noctalia.state.set('enabled', true)\n"
          "end\n"
      )
  );

  {
    ConfigService config;
    config.addPluginSource(
        {.kind = PluginSourceKind::Path,
         .name = "lifecycle-test",
         .location = (root / "path-plugins").string(),
         .enabled = true}
    );
    scripting::PluginManager manager(config);
    manager.refresh();
    // The plugin store reads install state straight off disk, so a plugin whose files
    // never landed must not report as materialized.
    TEST_CHECK(manager.isMaterialized("test/plugin"));
    TEST_CHECK(!manager.isMaterialized("test/absent"));
    TEST_CHECK(!manager.isMaterialized("not-an-id"));
    config.addReloadCallback([&] { manager.refresh(); });
    scripting::PluginServiceHost host(api, nullptr, nullptr, nullptr);
    host.start(config.config().plugins.pluginSettings);
    config.addReloadCallback([&] {
      if (config.lastChange().plugins) {
        host.refresh(config.config().plugins.pluginSettings);
      }
    });
    std::vector<std::string> enables;
    std::unique_ptr<scripting::ScriptRuntime> lifecycleRuntime;
    std::string lifecyclePluginId;
    config.addReloadCallback([&] {
      if (lifecycleRuntime != nullptr
          && !std::ranges::contains(config.config().plugins.enabled, std::string_view(lifecyclePluginId))) {
        lifecycleRuntime.reset();
      }
    });
    manager.setOnEnabled([&](std::string_view pluginId) {
      enables.emplace_back(pluginId);
      host.enablePlugin(pluginId);
      TEST_CHECK(std::ranges::contains(config.config().plugins.enabled, pluginId));
    });

    const auto firstEnable = manager.enable("test/plugin");
    TEST_CHECK(firstEnable.ok);
    TEST_CHECK(drainUntil([&] { return enables.size() == 1; }));
    TEST_CHECK(enables == std::vector<std::string>{"test/plugin"});

    {
      const auto path = root / "path-plugins/plugin/plugin.toml";
      std::ifstream input(path);
      TEST_CHECK(input.is_open());
      const std::string original{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
      const std::string provider = original
          + "[[launcher_provider]]\nid = \"search\"\n"
            "entry = \"launcher.luau\"\nprefix = \"one\"\n";
      int reconciliations = 0;
      manager.setOnChanged([&] { ++reconciliations; });
      config.forceReload();
      TEST_CHECK(reconciliations == 0);

      TEST_CHECK(writeText(path, provider));
      config.forceReload();
      TEST_CHECK(reconciliations == 1);
      TEST_CHECK(scripting::PluginRegistry::instance().hasEntry("test/plugin:search"));
      config.forceReload();
      TEST_CHECK(reconciliations == 1);

      TEST_CHECK(writeText(path.parent_path() / "launcher.luau", "function onQuery() end\n"));
      config.forceReload();
      TEST_CHECK(reconciliations == 1);

      // Content changes must not depend on timestamp granularity or file size.
      const auto timestamp = std::filesystem::last_write_time(path);
      std::string changed = provider;
      changed.replace(changed.find("one"), 3, "two");
      TEST_CHECK(writeText(path, changed));
      std::filesystem::last_write_time(path, timestamp);
      config.forceReload();
      TEST_CHECK(reconciliations == 2);
      const auto resolved = scripting::PluginRegistry::instance().resolve("test/plugin:search");
      TEST_CHECK(resolved.has_value() && resolved->entry->launcherPrefix == "two");

      TEST_CHECK(writeText(path, original));
      config.forceReload();
      TEST_CHECK(reconciliations == 3);
      TEST_CHECK(!scripting::PluginRegistry::instance().hasEntry("test/plugin:search"));
      TEST_CHECK(std::filesystem::remove(path));
      config.forceReload();
      TEST_CHECK(reconciliations == 4);
      TEST_CHECK(scripting::PluginRegistry::instance().findManifest("test/plugin") == nullptr);
      TEST_CHECK(writeText(path, original));
      config.forceReload();
      TEST_CHECK(reconciliations == 5);
      config.forceReload();
      TEST_CHECK(reconciliations == 5);
      manager.setOnChanged({});
    }

    lifecyclePluginId = "test/plugin";
    lifecycleRuntime =
        std::make_unique<scripting::ScriptRuntime>("test/plugin:widget", scripting::ScriptSettings{}, api, root);
    lifecycleRuntime->start(
        "=manager-disable",
        "noctalia.state.set('disable_loaded', true)\n"
        "function onExit(_signal, reason)\n"
        "  noctalia.state.set('disable_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/plugin", "disable_loaded"));
    manager.disable("test/plugin");
    TEST_CHECK(
        lifecycleRuntime == nullptr
        && scripting::PluginStateStore::instance().get("test/plugin", "disable_reason") == R"("disable")"
    );

    const auto secondEnable = manager.enable("test/plugin");
    TEST_CHECK(secondEnable.ok);
    TEST_CHECK(drainUntil([&] { return enables.size() == 2; }));
    TEST_CHECK(enables == std::vector<std::string>{"test/plugin", "test/plugin"});

    lifecyclePluginId = "test/plugin";
    lifecycleRuntime =
        std::make_unique<scripting::ScriptRuntime>("test/plugin:panel", scripting::ScriptSettings{}, api, root);
    lifecycleRuntime->start(
        "=manager-uninstall",
        "noctalia.state.set('uninstall_loaded', true)\n"
        "function onExit(_signal, reason)\n"
        "  noctalia.state.set('uninstall_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/plugin", "uninstall_loaded"));
    manager.remove("test/plugin");
    TEST_CHECK(
        lifecycleRuntime == nullptr
        && scripting::PluginStateStore::instance().get("test/plugin", "uninstall_reason") == R"("uninstall")"
    );

    const auto legacyEnable = manager.enable("test/api16");
    TEST_CHECK(legacyEnable.ok);
    TEST_CHECK(drainUntil([&] { return std::ranges::contains(config.config().plugins.enabled, "test/api16"); }));
    TEST_CHECK(waitForState("test/api16", "loaded"));
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    TEST_CHECK(!scripting::PluginStateStore::instance().get("test/api16", "enabled").has_value());
    lifecyclePluginId = "test/api16";
    lifecycleRuntime =
        std::make_unique<scripting::ScriptRuntime>("test/api16:widget", scripting::ScriptSettings{}, api, root);
    lifecycleRuntime->start(
        "=legacy-disable",
        "noctalia.state.set('legacy_loaded', true)\n"
        "function onExit(_signal, reason)\n"
        "  noctalia.state.set('exit_reason', reason)\n"
        "end\n",
        {}
    );
    TEST_CHECK(waitForState("test/api16", "legacy_loaded"));
    manager.disable("test/api16");
    TEST_CHECK(
        lifecycleRuntime == nullptr
        && scripting::PluginStateStore::instance().get("test/api16", "exit_reason") == R"("reload")"
    );

    const auto lifecycleEnable = manager.enable("test/api17");
    TEST_CHECK(lifecycleEnable.ok);
    TEST_CHECK(drainUntil([&] {
      return std::ranges::contains(config.config().plugins.enabled, "test/api17")
          && scripting::PluginStateStore::instance().get("test/api17", "enabled").has_value();
    }));
  }

  struct WallpaperMaskRequest {
    std::uint64_t ownerId = 0;
    std::string outputName;
    std::string path;
    std::string wallpaperPath;
  };
  std::vector<WallpaperMaskRequest> wallpaperMaskRequests;
  std::vector<std::uint64_t> clearedWallpaperMaskOwners;
  api.setWallpaperPaths({{"DP-1", "/wallpapers/current.png"}});
  api.setConfigSnapshot(std::make_shared<const toml::table>(toml::parse("[shell]\noffline_mode = true")));
  api.setWallpaperMaskHook([&](std::uint64_t ownerId, const std::string& outputName, const std::string& path,
                               const std::string& wallpaperPath) {
    wallpaperMaskRequests.push_back({
        .ownerId = ownerId,
        .outputName = outputName,
        .path = path,
        .wallpaperPath = wallpaperPath,
    });
  });
  api.setClearWallpaperMasksHook([&](std::uint64_t ownerId) { clearedWallpaperMaskOwners.push_back(ownerId); });
  const auto maskPluginDir = root / "wallpaper-mask-plugin";
  {
    scripting::ScriptRuntime runtime("test/wallpaper-mask:service", {}, api, maskPluginDir);
    runtime.start(
        "=wallpaper-mask",
        "assert(noctalia.getSetting('shell.offline_mode'))\n"
        "assert(noctalia.wallpaperPath('DP-1') == '/wallpapers/current.png')\n"
        "assert(noctalia.wallpaperPath('missing') == nil)\n"
        "noctalia.setWallpaperMask('DP-1', {\n"
        "  path = 'cache/mask.png',\n"
        "  wallpaperPath = '/wallpapers/current.png',\n"
        "})\n"
        "noctalia.setWallpaperMask('DP-1', nil)\n",
        {}
    );
    TEST_CHECK(drainUntil([&] { return wallpaperMaskRequests.size() == 2; }));
    const auto& setRequest = wallpaperMaskRequests[0];
    const auto& clearRequest = wallpaperMaskRequests[1];
    TEST_CHECK(
        setRequest.ownerId != 0
        && setRequest.outputName == "DP-1"
        && setRequest.path == (maskPluginDir / "cache/mask.png").string()
        && setRequest.wallpaperPath == "/wallpapers/current.png"
    );
    TEST_CHECK(
        clearRequest.ownerId == setRequest.ownerId
        && clearRequest.outputName == "DP-1"
        && clearRequest.path.empty()
        && clearRequest.wallpaperPath.empty()
    );
  }
  TEST_CHECK(drainUntil([&] { return clearedWallpaperMaskOwners.size() == 1; }));
  TEST_CHECK(clearedWallpaperMaskOwners.front() == wallpaperMaskRequests.front().ownerId);

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  return 0;
}
