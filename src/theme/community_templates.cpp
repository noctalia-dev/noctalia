#include "theme/community_templates.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "net/http_client.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace noctalia::theme {

  namespace {

    constexpr Logger kLog("community_templates");
    constexpr std::string_view kCatalogUrl = "https://api.noctalia.dev/templates";

    struct CommunityTemplateFile {
      std::string name;
      std::string md5;
    };

    struct CommunityTemplateEntry {
      std::string id;
      std::string inputPath;
      std::vector<std::string> outputPaths;
      std::string preHook;
      std::string postHook;
      std::optional<int> index;
    };

    struct CommunityTemplateInfo {
      std::string id;
      std::string displayName;
      std::string category;
      std::vector<CommunityTemplateFile> files;
      std::vector<CommunityTemplateEntry> entries;
    };

    std::filesystem::path catalogCachePath() { return communityTemplatesCacheDir() / "catalog.json"; }

    std::string stringField(const nlohmann::json& obj, std::string_view snake, std::string_view camel = {}) {
      auto read = [&](std::string_view key) -> std::string {
        if (key.empty())
          return {};
        auto it = obj.find(std::string(key));
        if (it == obj.end() || !it->is_string())
          return {};
        return it->get<std::string>();
      };
      std::string value = read(snake);
      if (value.empty())
        value = read(camel);
      return value;
    }

    std::string tomlEscape(std::string_view value) {
      std::string out;
      out.reserve(value.size());
      for (char ch : value) {
        switch (ch) {
        case '\\':
          out += "\\\\";
          break;
        case '"':
          out += "\\\"";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          out.push_back(ch);
          break;
        }
      }
      return out;
    }

    std::string tomlKey(std::string_view key) {
      const bool bare = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-';
      });
      if (bare)
        return std::string(key);
      return '"' + tomlEscape(key) + '"';
    }

    void writeTomlString(std::ostream& out, std::string_view key, std::string_view value) {
      out << key << " = \"" << tomlEscape(value) << "\"\n";
    }

    void writeOutputPaths(std::ostream& out, const std::vector<std::string>& paths) {
      if (paths.empty())
        return;
      if (paths.size() == 1) {
        writeTomlString(out, "output_path", paths.front());
        return;
      }
      out << "output_path = [";
      for (std::size_t i = 0; i < paths.size(); ++i) {
        if (i > 0)
          out << ", ";
        out << '"' << tomlEscape(paths[i]) << '"';
      }
      out << "]\n";
    }

    std::vector<std::string> readJsonStringArrayOrString(const nlohmann::json& obj, std::string_view key) {
      std::vector<std::string> out;
      auto it = obj.find(std::string(key));
      if (it == obj.end())
        return out;
      if (it->is_string()) {
        out.push_back(it->get<std::string>());
        return out;
      }
      if (!it->is_array())
        return out;
      for (const auto& item : *it) {
        if (item.is_string())
          out.push_back(item.get<std::string>());
      }
      return out;
    }

    std::optional<CommunityTemplateEntry> parseEntry(std::string_view fallbackId, const nlohmann::json& obj) {
      if (!obj.is_object())
        return std::nullopt;

      CommunityTemplateEntry entry;
      entry.id = stringField(obj, "id", "name");
      if (entry.id.empty())
        entry.id = std::string(fallbackId);
      entry.inputPath = stringField(obj, "input_path", "inputPath");
      entry.outputPaths = readJsonStringArrayOrString(obj, "output_path");
      if (entry.outputPaths.empty())
        entry.outputPaths = readJsonStringArrayOrString(obj, "outputPath");
      entry.preHook = stringField(obj, "pre_hook", "preHook");
      entry.postHook = stringField(obj, "post_hook", "postHook");
      if (auto index = obj.find("index"); index != obj.end() && index->is_number_integer())
        entry.index = index->get<int>();

      if (entry.id.empty() || entry.inputPath.empty())
        return std::nullopt;
      return entry;
    }

    std::vector<CommunityTemplateEntry> parseEntries(const nlohmann::json& obj) {
      std::vector<CommunityTemplateEntry> out;
      auto it = obj.find("templates");
      if (it == obj.end())
        return out;

      if (it->is_array()) {
        for (const auto& item : *it) {
          if (auto entry = parseEntry({}, item))
            out.push_back(std::move(*entry));
        }
      } else if (it->is_object()) {
        for (const auto& [key, value] : it->items()) {
          if (auto entry = parseEntry(key, value))
            out.push_back(std::move(*entry));
        }
      }
      return out;
    }

    std::vector<CommunityTemplateFile> parseFiles(const nlohmann::json& obj) {
      std::vector<CommunityTemplateFile> out;
      auto it = obj.find("files");
      if (it == obj.end() || !it->is_array())
        return out;
      for (const auto& item : *it) {
        if (!item.is_object())
          continue;
        CommunityTemplateFile file;
        file.name = stringField(item, "name");
        file.md5 = stringField(item, "md5");
        if (!file.name.empty())
          out.push_back(std::move(file));
      }
      return out;
    }

    std::optional<CommunityTemplateInfo> parseInfo(const nlohmann::json& obj) {
      if (!obj.is_object())
        return std::nullopt;
      CommunityTemplateInfo info;
      info.id = stringField(obj, "name", "id");
      if (info.id.empty() || !isSafeCommunityTemplateId(info.id))
        return std::nullopt;
      info.displayName = stringField(obj, "display_name", "displayName");
      if (info.displayName.empty())
        info.displayName = info.id;
      info.category = stringField(obj, "category");
      info.files = parseFiles(obj);
      info.entries = parseEntries(obj);
      return info;
    }

    std::vector<CommunityTemplateInfo> parseCatalogFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      if (!in)
        return {};
      try {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto root = nlohmann::json::parse(buf.str());
        const nlohmann::json* entries = &root;
        if (root.is_object()) {
          auto it = root.find("templates");
          if (it != root.end())
            entries = &*it;
        }
        if (!entries->is_array())
          return {};

        std::vector<CommunityTemplateInfo> out;
        for (const auto& item : *entries) {
          if (auto info = parseInfo(item))
            out.push_back(std::move(*info));
        }
        return out;
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community template catalog {}: {}", path.string(), e.what());
        return {};
      }
    }

    std::optional<CommunityTemplateInfo>
    findInfo(const std::vector<CommunityTemplateInfo>& catalog, std::string_view id) {
      auto it = std::find_if(catalog.begin(), catalog.end(), [id](const CommunityTemplateInfo& info) {
        return info.id == id;
      });
      if (it == catalog.end())
        return std::nullopt;
      return *it;
    }

    bool isSafeRelativePath(std::string_view raw) {
      if (raw.empty())
        return false;
      const std::filesystem::path path(raw);
      if (path.is_absolute())
        return false;
      for (const auto& part : path) {
        const std::string token = part.string();
        if (token == "." || token == ".." || token.empty())
          return false;
      }
      return true;
    }

    std::filesystem::path sidecarPath(const std::filesystem::path& path) {
      return path.parent_path() / (path.filename().string() + ".md5");
    }

    std::string readSmallFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      if (!in)
        return {};
      std::string value;
      std::getline(in, value);
      return value;
    }

    void writeSmallFile(const std::filesystem::path& path, std::string_view value) {
      std::ofstream out(path);
      if (out)
        out << value << '\n';
    }

    bool cacheMatches(const CommunityTemplateFile& file, const std::filesystem::path& dest) {
      if (!std::filesystem::exists(dest))
        return false;
      if (file.md5.empty())
        return true;
      return readSmallFile(sidecarPath(dest)) == file.md5;
    }

    std::string urlEncodePath(std::string_view path) {
      std::string out;
      std::size_t start = 0;
      while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (!out.empty())
          out.push_back('/');
        out += StringUtils::urlEncode(path.substr(start, end - start));
        if (slash == std::string_view::npos)
          break;
        start = slash + 1;
      }
      return out;
    }

    void writeGeneratedTemplateToml(const CommunityTemplateInfo& info) {
      if (info.entries.empty())
        return;

      const std::filesystem::path path = communityTemplateConfigPath(info.id);
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      std::ofstream out(path);
      if (!out) {
        kLog.warn("failed to write community template metadata {}", path.string());
        return;
      }

      out << "[catalog." << tomlKey(info.id) << "]\n";
      writeTomlString(out, "name", info.displayName);
      if (!info.category.empty())
        writeTomlString(out, "category", info.category);

      for (const auto& entry : info.entries) {
        out << "\n[templates." << tomlKey(entry.id) << "]\n";
        writeTomlString(out, "input_path", entry.inputPath);
        writeOutputPaths(out, entry.outputPaths);
        if (!entry.preHook.empty())
          writeTomlString(out, "pre_hook", entry.preHook);
        if (!entry.postHook.empty())
          writeTomlString(out, "post_hook", entry.postHook);
        if (entry.index.has_value())
          out << "index = " << *entry.index << "\n";
      }
    }

    std::optional<AvailableTemplate> readTemplateTomlInfo(const std::filesystem::path& path) {
      try {
        toml::table root = toml::parse_file(path.string());
        const toml::table* catalog = root["catalog"].as_table();
        if (catalog == nullptr || catalog->empty())
          return std::nullopt;
        const auto it = catalog->begin();
        AvailableTemplate out;
        out.id = std::string(it->first.str());
        out.displayName = out.id;
        if (const toml::table* info = it->second.as_table()) {
          if (const auto name = info->get_as<std::string>("name"))
            out.displayName = name->get();
          if (const auto category = info->get_as<std::string>("category"))
            out.category = category->get();
        }
        return out;
      } catch (const toml::parse_error&) {
        return std::nullopt;
      }
    }

  } // namespace

  CommunityTemplateService::CommunityTemplateService(HttpClient& httpClient) : m_httpClient(httpClient) {}

  void CommunityTemplateService::setReadyCallback(ReadyCallback callback) { m_readyCallback = std::move(callback); }

  void CommunityTemplateService::sync(const ThemeConfig::TemplatesConfig& templates) {
    const std::uint64_t generation = ++m_generation;
    if (!templates.enableCommunityTemplates) {
      return;
    }

    std::error_code ec;
    std::filesystem::create_directories(communityTemplatesCacheDir(), ec);
    if (!templates.communityIds.empty()) {
      syncSelectedFromCatalog(templates.communityIds, generation, false);
    }

    m_httpClient.download(
        kCatalogUrl, catalogCachePath(), [this, ids = templates.communityIds, generation](bool success) {
          if (generation != m_generation)
            return;
          if (!success) {
            kLog.warn("failed to refresh community template catalog; using cached metadata when available");
          }
          if (ids.empty()) {
            if (m_readyCallback) {
              DeferredCall::callLater([callback = m_readyCallback]() { callback(); });
            }
            return;
          }
          syncSelectedFromCatalog(ids, generation, success);
        }
    );
  }

  void CommunityTemplateService::syncSelectedFromCatalog(
      const std::vector<std::string>& selectedIds, std::uint64_t generation, bool notifyWhenReady
  ) {
    if (selectedIds.empty())
      return;

    const auto catalog = parseCatalogFile(catalogCachePath());
    if (catalog.empty())
      return;

    auto pending = std::make_shared<std::size_t>(0);
    auto completed = std::make_shared<std::size_t>(0);

    auto notifyIfReady = [this, generation, notifyWhenReady, pending, completed]() {
      if (!notifyWhenReady || generation != m_generation || *completed < *pending)
        return;
      if (m_readyCallback) {
        DeferredCall::callLater([callback = m_readyCallback]() { callback(); });
      }
    };

    std::unordered_set<std::string> seen;
    for (const auto& id : selectedIds) {
      if (!isSafeCommunityTemplateId(id) || !seen.insert(id).second)
        continue;
      auto info = findInfo(catalog, id);
      if (!info.has_value()) {
        kLog.warn("selected community template '{}' is not in the cached catalog", id);
        continue;
      }

      const std::filesystem::path dir = communityTemplateDir(id);
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (!info->entries.empty()) {
        writeGeneratedTemplateToml(*info);
      }

      for (const auto& file : info->files) {
        if (!isSafeRelativePath(file.name)) {
          kLog.warn("skipping unsafe community template path '{}' for '{}'", file.name, id);
          continue;
        }
        const std::filesystem::path dest = dir / std::filesystem::path(file.name);
        if (cacheMatches(file, dest))
          continue;
        if (dest.has_parent_path())
          std::filesystem::create_directories(dest.parent_path(), ec);
        ++(*pending);
        const std::string url =
            std::string(kCatalogUrl) + "/" + StringUtils::urlEncode(id) + "/" + urlEncodePath(file.name);
        m_httpClient.download(
            url, dest, [this, file, dest, generation, pending, completed, notifyIfReady](bool success) {
              ++(*completed);
              if (generation != m_generation)
                return;
              if (success) {
                if (!file.md5.empty())
                  writeSmallFile(sidecarPath(dest), file.md5);
              } else {
                kLog.warn("failed to download community template file {}", dest.string());
              }
              notifyIfReady();
            }
        );
      }
    }

    if (*pending == 0 && notifyWhenReady && m_readyCallback) {
      DeferredCall::callLater([callback = m_readyCallback]() { callback(); });
    }
  }

  std::vector<AvailableTemplate> CommunityTemplateService::availableTemplates() {
    std::vector<AvailableTemplate> out;
    const auto catalog = parseCatalogFile(catalogCachePath());
    out.reserve(catalog.size());
    for (const auto& info : catalog) {
      AvailableTemplate t;
      t.id = info.id;
      t.displayName = info.displayName.empty() ? info.id : info.displayName;
      t.category = info.category;
      out.push_back(std::move(t));
    }

    const std::filesystem::path cacheDir = communityTemplatesCacheDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(cacheDir, ec))
      return out;

    for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
      if (!entry.is_directory())
        continue;
      const auto toml = entry.path() / "template.toml";
      if (!std::filesystem::exists(toml))
        continue;
      if (auto info = readTemplateTomlInfo(toml)) {
        auto exists = std::any_of(out.begin(), out.end(), [&](const AvailableTemplate& t) { return t.id == info->id; });
        if (!exists)
          out.push_back(std::move(*info));
      }
    }

    std::sort(out.begin(), out.end(), [](const AvailableTemplate& a, const AvailableTemplate& b) {
      if (a.category != b.category)
        return a.category < b.category;
      if (a.displayName != b.displayName)
        return a.displayName < b.displayName;
      return a.id < b.id;
    });
    return out;
  }

  std::filesystem::path communityTemplatesCacheDir() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0')
      return std::filesystem::path(xdg) / "noctalia" / "community-templates";
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
      return std::filesystem::path(home) / ".cache" / "noctalia" / "community-templates";
    return std::filesystem::path("/tmp") / "noctalia" / "community-templates";
  }

  std::filesystem::path communityTemplateDir(std::string_view id) {
    return communityTemplatesCacheDir() / std::string(id);
  }

  std::filesystem::path communityTemplateConfigPath(std::string_view id) {
    return communityTemplateDir(id) / "template.toml";
  }

  bool isSafeCommunityTemplateId(std::string_view id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
  }

} // namespace noctalia::theme
