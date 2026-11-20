#pragma once

#include "core/files/file_watcher.h"

#include <filesystem>
#include <span>
#include <vector>

namespace scripting {

  class PluginScriptWatcher {
  public:
    PluginScriptWatcher() = default;
    ~PluginScriptWatcher();

    PluginScriptWatcher(const PluginScriptWatcher&) = delete;
    PluginScriptWatcher& operator=(const PluginScriptWatcher&) = delete;

    void start(FileWatcher* watcher, std::filesystem::path entryPath, FileWatcher::Callback callback);
    void stop();
    void setModulePaths(std::span<const std::filesystem::path> paths);

  private:
    void rebuild();

    FileWatcher* m_watcher = nullptr;
    std::filesystem::path m_entryPath;
    FileWatcher::Callback m_callback;
    std::vector<std::filesystem::path> m_modulePaths;
    std::vector<FileWatcher::WatchId> m_watchIds;
    bool m_active = false;
  };

} // namespace scripting
