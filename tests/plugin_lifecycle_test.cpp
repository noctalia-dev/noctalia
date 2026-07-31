#include "config/config_service.h"
#include "core/deferred_call.h"
#include "scripting/plugin_manager.h"
#include "scripting/plugin_registry.h"
#include "scripting/plugin_service_host.h"
#include "scripting/plugin_state_store.h"
#include "scripting/script_api_context.h"
#include "scripting/script_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_lifecycle_test: {}", message);
    }
    return condition;
  }

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
  bool ok = true;

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
    ok = expect(waitForState("test/enable", "loaded"), "enable service did not load") && ok;
    ok = expect(
             !scripting::PluginStateStore::instance().get("test/enable", "enabled").has_value(),
             "service load invoked onEnable without an explicit enable"
         )
        && ok;
    ok = expect(runtime.enqueueCall("onEnable", {}), "could not enqueue onEnable") && ok;
    ok = expect(waitForState("test/enable", "enabled"), "onEnable was not delivered") && ok;
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
    ok = expect(waitForState("test/deactivate", "loaded"), "service did not load") && ok;
    runtime.stop(scripting::ScriptExitReason::Disable);
  }

  ok = expect(
           scripting::PluginStateStore::instance().get("test/deactivate", "exit_signal") == "0",
           "disable changed the existing numeric exit signal"
       )
      && ok;
  ok = expect(
           scripting::PluginStateStore::instance().get("test/deactivate", "exit_reason") == R"("disable")",
           "onExit did not receive the disable reason"
       )
      && ok;

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
    ok = expect(waitForState("test/remove", "loaded"), "removal service did not load") && ok;
    runtime.stop(scripting::ScriptExitReason::Uninstall);
  }

  ok = expect(
           scripting::PluginStateStore::instance().get("test/remove", "exit_signal") == "0",
           "remove changed the existing numeric exit signal"
       )
      && ok;
  ok = expect(
           scripting::PluginStateStore::instance().get("test/remove", "exit_reason") == R"("uninstall")",
           "onExit did not receive the uninstall reason"
       )
      && ok;

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
    ok = expect(waitForState("test/ordinary-stop", "loaded"), "ordinary service did not load") && ok;
    runtime.stop();
  }

  ok = expect(
           scripting::PluginStateStore::instance().get("test/ordinary-stop", "exit_signal") == "0"
               && scripting::PluginStateStore::instance().get("test/ordinary-stop", "exit_reason") == R"("reload")",
           "ordinary runtime teardown reported the wrong exit context"
       )
      && ok;

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
    ok = expect(waitForState("test/non-service", "loaded"), "non-service runtime did not load") && ok;
    runtime.stop();
  }

  ok = expect(
           scripting::PluginStateStore::instance().get("test/non-service", "exit_reason") == R"("disable")",
           "scoped plugin reason did not reach a non-service runtime"
       )
      && ok;

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
    ok = expect(waitForState("test/reload", "loaded"), "reload service did not load") && ok;
    runtime.reload("=after-reload", "noctalia.state.set('reloaded', true)\n", {});
    ok = expect(waitForState("test/reload", "reloaded"), "service did not reload") && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/reload", "exit_signal") == "0"
                 && scripting::PluginStateStore::instance().get("test/reload", "exit_reason") == R"("reload")",
             "runtime reload reported the wrong exit context"
         )
        && ok;
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
    ok = expect(waitForState("test/shutdown", "loaded"), "shutdown service did not load") && ok;
    scripting::ScriptRuntime::setShutdownSignal(15);
    runtime.stop();
    scripting::ScriptRuntime::setShutdownSignal(0);
  }

  ok = expect(
           scripting::PluginStateStore::instance().get("test/shutdown", "exit_signal") == "15"
               && scripting::PluginStateStore::instance().get("test/shutdown", "exit_reason") == R"("shutdown")",
           "graceful shutdown reported the wrong exit context"
       )
      && ok;

  const auto root =
      std::filesystem::temp_directory_path() / ("noctalia-plugin-lifecycle-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
  std::filesystem::create_directories(root / "state");
  std::filesystem::create_directories(root / "data");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

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
    ok =
        expect(
            drainUntil([&] {
              return scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_error_nil").has_value()
                  && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_error").has_value()
                  && soundPlays.size() == 1;
            }),
            "sound load callbacks were not delivered"
        )
        && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_accepted") == "true"
                 && scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_ok") == "true"
                 && scripting::PluginStateStore::instance().get("test/sound-outcomes", "success_error_nil") == "true",
             "successful sound load returned the wrong callback payload"
         )
        && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_accepted") == "true"
                 && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_ok") == "false"
                 && scripting::PluginStateStore::instance().get("test/sound-outcomes", "failure_error")
                     == R"("decode failed")",
             "failed sound load returned the wrong callback payload"
         )
        && ok;
    const auto clickLoad =
        std::ranges::find_if(soundLoads, [](const SoundLoadRequest& request) { return request.name == "click"; });
    ok = expect(
             clickLoad != soundLoads.end() && clickLoad->path == (soundPluginDir / "sounds/click.wav").string(),
             "relative sound path did not resolve against the plugin directory"
         )
        && ok;
    ok = expect(
             soundPlays.size() == 1
                 && clickLoad != soundLoads.end()
                 && soundPlays.front().ownerId == clickLoad->ownerId
                 && soundPlays.front().name == "click",
             "successful sound callback did not play from its own sound bank"
         )
        && ok;
  }
  ok = expect(
           drainUntil([&] { return unloadedSoundOwners.size() == 1; }),
           "sound outcome runtime did not unload its sound bank"
       )
      && ok;

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
    ok = expect(waitForState("test/sound-limits", "ninth_accepted"), "sound pending-limit script did not run") && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/sound-limits", "accepted_count") == "8"
                 && scripting::PluginStateStore::instance().get("test/sound-limits", "duplicate_accepted") == "false"
                 && scripting::PluginStateStore::instance().get("test/sound-limits", "ninth_accepted") == "false",
             "sound pending limits accepted a duplicate name or ninth load"
         )
        && ok;
    ok = expect(drainUntil([&] { return soundLoads.size() == 8; }), "accepted pending sound loads were not dispatched")
        && ok;
  }
  ok = expect(
           drainUntil([&] { return unloadedSoundOwners.size() == 1; }),
           "sound limit runtime did not unload its sound bank"
       )
      && ok;

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
    ok = expect(
             drainUntil([&] {
               return runtime.unhealthy()
                   && scripting::PluginStateStore::instance()
                          .get("test/sound-unhealthy", "completion_after_unhealthy")
                          .has_value();
             }),
             "accepted sound completion was dropped after the runtime became unhealthy"
         )
        && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/sound-unhealthy", "after_errors_accepted") == "true"
                 && scripting::PluginStateStore::instance().get("test/sound-unhealthy", "completion_after_unhealthy")
                     == "true",
             "sound completion after five failing callbacks had the wrong payload"
         )
        && ok;
  }
  ok = expect(
           drainUntil([&] { return unloadedSoundOwners.size() == 1; }),
           "unhealthy sound runtime did not unload its sound bank"
       )
      && ok;

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
    ok = expect(
             drainUntil([&] { return soundLoads.size() == 2 && soundPlays.size() == 2; }),
             "concurrent sound runtimes did not load and play"
         )
        && ok;
    if (soundLoads.size() == 2) {
      firstOwner = soundLoads[0].ownerId;
      secondOwner = soundLoads[1].ownerId;
    }
    ok = expect(
             firstOwner != 0 && secondOwner != 0 && firstOwner != secondOwner,
             "concurrent sound runtimes reused a host id"
         )
        && ok;
    ok = expect(
             std::ranges::count_if(
                 soundPlays,
                 [firstOwner](const SoundPlayRequest& request) {
                   return request.ownerId == firstOwner && request.name == "click";
                 }
             ) == 1
                 && std::ranges::count_if(
                        soundPlays,
                        [secondOwner](const SoundPlayRequest& request) {
                          return request.ownerId == secondOwner && request.name == "click";
                        }
                    ) == 1,
             "concurrent runtimes did not keep same-named sounds isolated"
         )
        && ok;
  }
  ok = expect(
           drainUntil([&] {
             return std::ranges::count(unloadedSoundOwners, firstOwner) == 1
                 && std::ranges::count(unloadedSoundOwners, secondOwner) == 1;
           }),
           "concurrent sound runtimes did not unload exactly once per owner"
       )
      && ok;

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
    ok = expect(
             drainUntil([&] {
               if (!soundLoads.empty()) {
                 staleOwner = soundLoads.front().ownerId;
               }
               return staleOwner != 0
                   && scripting::PluginStateStore::instance().get("test/sound-stale", "replacement_loaded").has_value()
                   && std::ranges::count(unloadedSoundOwners, staleOwner) == 1;
             }),
             "sound runtime reload did not replace and unload the old host"
         )
        && ok;
    ok = expect(
             scripting::PluginStateStore::instance().get("test/sound-stale", "stale_callback_reached") == "false",
             "stale sound completion reached the replacement host"
         )
        && ok;
  }
  (void)drainUntil([&] { return unloadedSoundOwners.size() >= 2; });
  ok = expect(
           writeText(
               root / "path-plugins/plugin/plugin.toml",
               "id = \"test/plugin\"\n"
               "name = \"Lifecycle test\"\n"
               "version = \"1\"\n"
               "plugin_api = 17\n"
           ),
           "failed to create local plugin fixture"
       )
      && ok;
  ok = expect(
           writeText(
               root / "path-plugins/api16/plugin.toml",
               "id = \"test/api16\"\n"
               "name = \"API 16 service\"\n"
               "version = \"1\"\n"
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
                   "version = \"1\"\n"
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
               ),
           "failed to create service API fixtures"
       )
      && ok;

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
    config.addReloadCallback([&]() { manager.refresh(); });
    scripting::PluginServiceHost host(api, nullptr, nullptr, nullptr);
    host.start(config.config().plugins.pluginSettings);
    config.addReloadCallback([&]() {
      if (config.lastChange().plugins) {
        host.refresh(config.config().plugins.pluginSettings);
      }
    });
    std::vector<std::string> enables;
    std::unique_ptr<scripting::ScriptRuntime> lifecycleRuntime;
    std::string lifecyclePluginId;
    config.addReloadCallback([&]() {
      if (lifecycleRuntime != nullptr
          && !std::ranges::contains(config.config().plugins.enabled, std::string_view(lifecyclePluginId))) {
        lifecycleRuntime.reset();
      }
    });
    manager.setOnEnabled([&](std::string_view pluginId) {
      enables.emplace_back(pluginId);
      host.enablePlugin(pluginId);
      ok = expect(
               std::ranges::contains(config.config().plugins.enabled, pluginId),
               "activation callback ran before the plugin was enabled"
           )
          && ok;
    });

    const auto firstEnable = manager.enable("test/plugin");
    ok = expect(firstEnable.ok, "first enable request failed") && ok;
    ok = expect(drainUntil([&] { return enables.size() == 1; }), "first enable did not publish an enable event") && ok;
    ok = expect(
             enables == std::vector<std::string>{"test/plugin"}, "first enable did not publish exactly one enable event"
         )
        && ok;

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
    ok = expect(waitForState("test/plugin", "disable_loaded"), "manager disable runtime did not load") && ok;
    manager.disable("test/plugin");
    ok = expect(
             lifecycleRuntime == nullptr
                 && scripting::PluginStateStore::instance().get("test/plugin", "disable_reason") == R"("disable")",
             "manager disable did not propagate its reason through config teardown"
         )
        && ok;

    const auto secondEnable = manager.enable("test/plugin");
    ok = expect(secondEnable.ok, "second enable request failed") && ok;
    ok = expect(drainUntil([&] { return enables.size() == 2; }), "second enable did not publish an enable event") && ok;
    ok = expect(
             enables == std::vector<std::string>{"test/plugin", "test/plugin"},
             "re-enable did not publish exactly one enable event"
         )
        && ok;

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
    ok = expect(waitForState("test/plugin", "uninstall_loaded"), "manager uninstall runtime did not load") && ok;
    manager.remove("test/plugin");
    ok = expect(
             lifecycleRuntime == nullptr
                 && scripting::PluginStateStore::instance().get("test/plugin", "uninstall_reason") == R"("uninstall")",
             "manager uninstall did not propagate its reason through config teardown"
         )
        && ok;

    const auto legacyEnable = manager.enable("test/api16");
    ok = expect(legacyEnable.ok, "API 16 enable request failed") && ok;
    ok = expect(
             drainUntil([&] { return std::ranges::contains(config.config().plugins.enabled, "test/api16"); }),
             "API 16 plugin was not enabled"
         )
        && ok;
    ok = expect(waitForState("test/api16", "loaded"), "API 16 service did not load through manager refresh") && ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ok = expect(
             !scripting::PluginStateStore::instance().get("test/api16", "enabled").has_value(),
             "service below API 17 received onEnable"
         )
        && ok;
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
    ok = expect(waitForState("test/api16", "legacy_loaded"), "API 16 lifecycle runtime did not load") && ok;
    manager.disable("test/api16");
    ok = expect(
             lifecycleRuntime == nullptr
                 && scripting::PluginStateStore::instance().get("test/api16", "exit_reason") == R"("reload")",
             "plugin below API 17 received an explicit lifecycle reason"
         )
        && ok;

    const auto lifecycleEnable = manager.enable("test/api17");
    ok = expect(lifecycleEnable.ok, "API 17 enable request failed") && ok;
    ok = expect(
             drainUntil([&] {
               return std::ranges::contains(config.config().plugins.enabled, "test/api17")
                   && scripting::PluginStateStore::instance().get("test/api17", "enabled").has_value();
             }),
             "manager enable did not deliver onEnable to the API 17 service after refresh"
         )
        && ok;
  }

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  std::filesystem::remove_all(root);
  return ok ? 0 : 1;
}
