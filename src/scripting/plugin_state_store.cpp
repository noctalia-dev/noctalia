#include "scripting/plugin_state_store.h"

#include <utility>

namespace scripting {

  PluginStateStore& PluginStateStore::instance() {
    static PluginStateStore store;
    return store;
  }

  void PluginStateStore::set(const std::string& pluginId, const std::string& key, std::string json) {
    std::vector<Watcher> toNotify;
    {
      std::scoped_lock lock(m_mutex);
      PluginState& state = m_plugins[pluginId];
      state.values[key] = json; // store a copy; `json` is kept for notification below
      if (const auto it = state.watchers.find(key); it != state.watchers.end()) {
        toNotify.reserve(it->second.size());
        for (const WatcherEntry& entry : it->second) {
          toNotify.push_back(entry.watcher);
        }
      }
    }
    // Notify outside the lock: a watcher enqueues onto a runtime's mailbox, and
    // holding the store mutex across that could invert lock order with set().
    for (const Watcher& watcher : toNotify) {
      watcher(json);
    }
  }

  std::optional<std::string> PluginStateStore::get(const std::string& pluginId, const std::string& key) const {
    std::scoped_lock lock(m_mutex);
    const auto pit = m_plugins.find(pluginId);
    if (pit == m_plugins.end()) {
      return std::nullopt;
    }
    const auto vit = pit->second.values.find(key);
    if (vit == pit->second.values.end()) {
      return std::nullopt;
    }
    return vit->second;
  }

  void
  PluginStateStore::watch(const std::string& pluginId, const std::string& key, std::uint64_t token, Watcher watcher) {
    std::scoped_lock lock(m_mutex);
    m_plugins[pluginId].watchers[key].push_back(WatcherEntry{.token = token, .watcher = std::move(watcher)});
  }

  void PluginStateStore::removeWatchers(std::uint64_t token) {
    std::scoped_lock lock(m_mutex);
    for (auto& [pluginId, state] : m_plugins) {
      for (auto& [key, list] : state.watchers) {
        std::erase_if(list, [token](const WatcherEntry& entry) { return entry.token == token; });
      }
    }
  }

} // namespace scripting
