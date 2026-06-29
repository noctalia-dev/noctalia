#include "scripting/plugin_file_cache.h"

#include "config/config_types.h"
#include "core/deferred_call.h"
#include "scripting/plugin_git.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_source_paths.h"
#include "util/file_utils.h"

#include <filesystem>
#include <fstream>
#include <thread>

namespace scripting {

  namespace {
    std::string cacheKey(const std::string& sourceName, const std::string& pluginId, const std::string& filename) {
      return sourceName + "/" + pluginId + "/" + filename;
    }
  } // namespace

  std::string PluginFileCache::cacheFilePath(
      const std::string& sourceName, const std::string& pluginSubdir, const std::string& filename
  ) {
    const std::string base = FileUtils::stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugin-cache/" + sourceName + "/" + pluginSubdir + "/" + filename;
  }

  void PluginFileCache::setOnReady(ReadyCallback cb) { m_onReady = std::move(cb); }

  void PluginFileCache::invalidateSource(const std::string& sourceName) {
    std::scoped_lock lock(m_mutex);
    std::erase_if(m_missing, [&](const std::string& key) { return key.starts_with(sourceName + "/"); });
    std::erase_if(m_inFlight, [&](const std::string& key) { return key.starts_with(sourceName + "/"); });
  }

  std::string
  PluginFileCache::resolve(const std::string& pluginId, const PluginSourceConfig& source, const std::string& filename) {
    const auto subdir = pluginSubdirFromId(pluginId);
    if (!subdir.has_value()) {
      return {};
    }
    const std::string key = cacheKey(source.name, pluginId, filename);

    {
      std::scoped_lock lock(m_mutex);
      if (m_missing.contains(key)) {
        return {};
      }
    }

    // Materialized plugins: the file is already on disk.
    if (source.kind == PluginSourceKind::Git) {
      const auto materializedRoot = plugin_paths::gitMaterializedRoot(source);
      if (!materializedRoot.empty()) {
        const auto materialized = materializedRoot / *subdir / filename;
        if (std::filesystem::exists(materialized)) {
          return materialized.string();
        }
      }
    }

    // Path sources: read directly from the source directory.
    if (source.kind == PluginSourceKind::Path) {
      const auto path = std::filesystem::path(source.location) / *subdir / filename;
      if (std::filesystem::exists(path)) {
        return path.string();
      }
      std::scoped_lock lock(m_mutex);
      m_missing.insert(key);
      return {};
    }

    // Git sources: check cache, then background-fetch.
    const std::string cached = cacheFilePath(source.name, *subdir, filename);
    if (!cached.empty() && std::filesystem::exists(cached)) {
      return cached;
    }

    {
      std::scoped_lock lock(m_mutex);
      if (m_inFlight.contains(key)) {
        return {};
      }
      m_inFlight.insert(key);
    }

    const auto repoRoot = plugin_paths::gitRepoRoot(source);
    std::thread([this, pluginId, key, repoPath = *subdir + "/" + filename, repoRoot, cached, filename]() {
      const auto result = plugin_git::showFile(repoRoot, repoPath);
      if (!result.ok || result.out.empty()) {
        std::scoped_lock lock(m_mutex);
        m_inFlight.erase(key);
        m_missing.insert(key);
        return;
      }

      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(cached).parent_path(), ec);
      if (!ec) {
        std::ofstream out(cached, std::ios::binary);
        out.write(result.out.data(), static_cast<std::streamsize>(result.out.size()));
      }

      const std::string resolvedPath = (!ec && std::filesystem::exists(cached)) ? cached : std::string{};

      DeferredCall::callLater([this, pluginId, filename, resolvedPath, key]() {
        {
          std::scoped_lock lock(m_mutex);
          m_inFlight.erase(key);
        }
        if (!resolvedPath.empty() && m_onReady) {
          m_onReady(pluginId, filename, resolvedPath);
        }
      });
    }).detach();

    return {};
  }

} // namespace scripting
