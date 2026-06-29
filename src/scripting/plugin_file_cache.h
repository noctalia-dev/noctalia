#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

struct PluginSourceConfig;

namespace scripting {

  class PluginFileCache {
  public:
    using ReadyCallback =
        std::function<void(const std::string& pluginId, const std::string& filename, const std::string& path)>;

    // Returns the resolved path if the file is already available (cached or on disk).
    // Returns empty if a background fetch was started (onReady fires when it lands)
    // or the file is known to not exist in the source.
    std::string resolve(const std::string& pluginId, const PluginSourceConfig& source, const std::string& filename);

    void setOnReady(ReadyCallback cb);

    void invalidateSource(const std::string& sourceName);

  private:
    [[nodiscard]] static std::string
    cacheFilePath(const std::string& sourceName, const std::string& pluginSubdir, const std::string& filename);

    ReadyCallback m_onReady;
    std::mutex m_mutex;
    std::unordered_set<std::string> m_inFlight;
    std::unordered_set<std::string> m_missing;
  };

} // namespace scripting
