#include "shell/wallpaper/wallpaper_shuffle_state.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace wallpaper {

  void ShuffleState::setStatePath(std::filesystem::path path) {
    m_statePath = std::move(path);
    m_seenByScope.clear();
    load();
  }

  std::string ShuffleState::pick(
      std::string_view scope, std::string_view source, const std::vector<std::string>& candidates,
      std::string_view currentPath, float randomUnit
  ) {
    if (candidates.empty()) {
      return {};
    }

    auto& seen = m_seenByScope[std::string(scope)][std::string(source)];
    std::unordered_set<std::string_view> candidateSet;
    candidateSet.reserve(candidates.size());
    for (const std::string& candidate : candidates) {
      candidateSet.insert(candidate);
    }

    std::erase_if(seen, [&](const std::string& path) { return !candidateSet.contains(path); });
    if (candidateSet.contains(currentPath)) {
      seen.emplace(currentPath);
    }

    std::vector<const std::string*> available;
    available.reserve(candidates.size());
    const auto collectAvailable = [&] {
      available.clear();
      for (const std::string& candidate : candidates) {
        if (candidate != currentPath && !seen.contains(candidate)) {
          available.push_back(&candidate);
        }
      }
    };
    collectAvailable();

    if (available.empty()) {
      seen.clear();
      if (candidateSet.contains(currentPath)) {
        seen.emplace(currentPath);
      }
      collectAvailable();
    }

    if (available.empty()) {
      save();
      return candidates.front();
    }

    if (!std::isfinite(randomUnit)) {
      randomUnit = 0.0F;
    }
    randomUnit = std::clamp(randomUnit, 0.0F, 1.0F);
    const std::size_t index =
        std::min(static_cast<std::size_t>(randomUnit * static_cast<float>(available.size())), available.size() - 1);
    const std::string picked = *available[index];
    seen.insert(picked);
    save();
    return picked;
  }

  void ShuffleState::load() {
    if (m_statePath.empty()) {
      return;
    }

    std::ifstream file(m_statePath);
    if (!file.is_open()) {
      return;
    }

    try {
      const nlohmann::json root = nlohmann::json::parse(file);
      if (root.value("version", 0) != 2) {
        return;
      }
      const auto scopes = root.find("scopes");
      if (scopes == root.end() || !scopes->is_object()) {
        return;
      }
      for (const auto& [scope, sources] : scopes->items()) {
        if (!sources.is_object()) {
          continue;
        }
        auto& seenBySource = m_seenByScope[scope];
        for (const auto& [source, paths] : sources.items()) {
          if (!paths.is_array()) {
            continue;
          }
          auto& seen = seenBySource[source];
          for (const auto& path : paths) {
            if (path.is_string()) {
              seen.insert(path.get<std::string>());
            }
          }
        }
      }
    } catch (const nlohmann::json::exception&) {
      m_seenByScope.clear();
    }
  }

  void ShuffleState::save() const {
    if (m_statePath.empty()) {
      return;
    }

    std::error_code ec;
    std::filesystem::create_directories(m_statePath.parent_path(), ec);
    if (ec) {
      return;
    }

    try {
      nlohmann::json scopes = nlohmann::json::object();
      for (const auto& [scope, seenBySource] : m_seenByScope) {
        nlohmann::json sources = nlohmann::json::object();
        for (const auto& [source, seen] : seenBySource) {
          sources[source] = seen;
        }
        scopes[scope] = std::move(sources);
      }
      const nlohmann::json root{{"version", 2}, {"scopes", std::move(scopes)}};
      std::ofstream file(m_statePath, std::ios::trunc);
      if (file.is_open()) {
        file << root.dump(2) << '\n';
      }
    } catch (const nlohmann::json::exception&) {
      // A non-UTF-8 filesystem path cannot be represented in JSON. Runtime state still works.
    }
  }

} // namespace wallpaper
