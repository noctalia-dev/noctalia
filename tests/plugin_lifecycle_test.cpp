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
