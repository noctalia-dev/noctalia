#include "scripting/plugin_script_watcher.h"

#include <algorithm>

namespace scripting {

  PluginScriptWatcher::~PluginScriptWatcher() { stop(); }

  void
  PluginScriptWatcher::start(FileWatcher* watcher, std::filesystem::path entryPath, FileWatcher::Callback callback) {
    stop();
    m_watcher = watcher;
    m_entryPath = std::move(entryPath);
    m_callback = std::move(callback);
    m_active = true;
    rebuild();
  }

  void PluginScriptWatcher::stop() {
    if (m_watcher != nullptr) {
      for (const auto id : m_watchIds) {
        m_watcher->unwatch(id);
      }
    }
    m_watchIds.clear();
    m_active = false;
  }

  void PluginScriptWatcher::setModulePaths(std::span<const std::filesystem::path> paths) {
    std::vector<std::filesystem::path> next(paths.begin(), paths.end());
    std::ranges::sort(next);
    next.erase(std::ranges::unique(next).begin(), next.end());
    if (next == m_modulePaths) {
      return;
    }
    m_modulePaths = std::move(next);
    rebuild();
  }

  void PluginScriptWatcher::rebuild() {
    if (!m_active || m_watcher == nullptr || m_entryPath.empty() || !m_callback) {
      return;
    }
    for (const auto id : m_watchIds) {
      m_watcher->unwatch(id);
    }
    m_watchIds.clear();

    const auto addWatch = [this](const std::filesystem::path& path) {
      const auto id = m_watcher->watch(path, m_callback, FileWatcher::WatchTrigger::WriteCompleted);
      if (id != 0) {
        m_watchIds.push_back(id);
      }
    };
    addWatch(m_entryPath);
    for (const auto& path : m_modulePaths) {
      if (path != m_entryPath) {
        addWatch(path);
      }
    }
  }

} // namespace scripting
