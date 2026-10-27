#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scripting {

  // Per-plugin shared key/value store with push notifications. Values are JSON
  // strings (serialized Lua values) so they cross runtime/thread boundaries by
  // copy. Scoped per plugin id — a plugin can never see another plugin's state.
  //
  // This is how isolated entry runtimes of the same plugin (a service, its
  // widgets, its panels) share data without sharing Lua memory.
  class PluginStateStore {
  public:
    static PluginStateStore& instance();

    // Invoked with the new JSON value when a watched key changes. Runs on the
    // setter's thread, so a watcher must only marshal the value onto its own
    // runtime (never touch a foreign Lua state directly).
    using Watcher = std::function<void(const std::string& json)>;

    void set(const std::string& pluginId, const std::string& key, const std::string& json);
    [[nodiscard]] std::optional<std::string> get(const std::string& pluginId, const std::string& key) const;

    // Register a watcher for (pluginId, key), tagged with `token` (the watching
    // runtime's identity) so it can be cleaned up on reload/stop.
    void watch(const std::string& pluginId, const std::string& key, std::uint64_t token, Watcher watcher);

    // Drop every watcher tagged with `token`.
    void removeWatchers(std::uint64_t token);

  private:
    struct WatcherEntry {
      std::uint64_t token = 0;
      Watcher watcher;
    };
    struct PluginState {
      std::unordered_map<std::string, std::string> values;
      std::unordered_map<std::string, std::vector<WatcherEntry>> watchers;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PluginState> m_plugins;
  };

} // namespace scripting
