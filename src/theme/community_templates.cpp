#include "theme/community_templates.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "net/http_client.h"
#include "util/checksum.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
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
    constexpr std::string_view kCacheMetadataFilename = ".noctalia-cache.json";

    struct CommunityTemplateFile {
      std::string name;
      std::string md5;
      std::optional<std::filesystem::perms> mode;
    };

    struct CommunityTemplateEntry {
      std::string id;
      std::string inputPath;
      std::vector<std::string> outputPaths;
      std::string preHook;
      std::string postHook;
      std::string postAction;
      std::optional<int> index;
    };

    struct CommunityTemplateInfo {
      std::string id;
      std::string displayName;
      std::string category;
      std::vector<CommunityTemplateFile> files;
      std::vector<CommunityTemplateEntry> entries;
    };

    struct CacheMetadata {
      std::map<std::string, std::string> fileMd5s;
    };

    std::filesystem::path catalogCachePath() { return communityTemplatesCacheDir() / "catalog.json"; }

    std::filesystem::path cacheMetadataPath(const std::filesystem::path& dir) {
      return dir / std::string(kCacheMetadataFilename);
    }

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
      const bool bare = !key.empty()
          && std::ranges::all_of(key, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_' || ch == '-'; });
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
      entry.postAction = stringField(obj, "post_action", "postAction");
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

    std::optional<std::filesystem::perms> parseFileMode(std::string_view mode) {
      if (mode.size() != 4)
        return std::nullopt;

      unsigned int value = 0;
      for (char ch : mode) {
        if (ch < '0' || ch > '7')
          return std::nullopt;
        value = (value << 3U) | static_cast<unsigned int>(ch - '0');
      }
      if ((value & 07000U) != 0)
        return std::nullopt;
      return static_cast<std::filesystem::perms>(value);
    }

    std::vector<CommunityTemplateFile> parseFiles(std::string_view templateId, const nlohmann::json& obj) {
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
        if (file.name.empty())
          continue;

        const std::string mode = stringField(item, "mode");
        if (!mode.empty()) {
          if (auto parsed = parseFileMode(mode)) {
            file.mode = parsed;
          } else {
            kLog.warn("community template '{}' file '{}' has invalid mode metadata '{}'", templateId, file.name, mode);
          }
        }
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
      info.files = parseFiles(info.id, obj);
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
      auto it = std::ranges::find(catalog, id, &CommunityTemplateInfo::id);
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

    std::optional<std::string> readTextFile(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in)
        return std::nullopt;
      std::stringstream buf;
      buf << in.rdbuf();
      return buf.str();
    }

    bool writeTextFile(const std::filesystem::path& path, std::string_view text) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      std::ofstream out(path, std::ios::binary);
      if (!out)
        return false;
      out << text;
      return static_cast<bool>(out);
    }

    std::optional<std::string> readLegacyMd5Sidecar(const std::filesystem::path& path) {
      std::ifstream in(sidecarPath(path));
      if (!in)
        return std::nullopt;
      std::string value;
      std::getline(in, value);
      value = StringUtils::toLower(StringUtils::trim(value));
      if (value.empty())
        return std::nullopt;
      return value;
    }

    void removeLegacyMd5Sidecar(const std::filesystem::path& path) {
      std::error_code ec;
      std::filesystem::remove(sidecarPath(path), ec);
    }

    CacheMetadata readCacheMetadata(const std::filesystem::path& dir) {
      CacheMetadata metadata;
      const std::filesystem::path path = cacheMetadataPath(dir);
      std::ifstream in(path);
      if (!in)
        return metadata;

      try {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto root = nlohmann::json::parse(buf.str());
        const auto files = root.find("files");
        if (files == root.end() || !files->is_object())
          return metadata;

        for (const auto& [name, value] : files->items()) {
          std::string md5;
          if (value.is_string()) {
            md5 = value.get<std::string>();
          } else if (value.is_object()) {
            md5 = stringField(value, "md5");
          }
          md5 = StringUtils::toLower(StringUtils::trim(md5));
          if (!name.empty() && !md5.empty()) {
            metadata.fileMd5s[name] = std::move(md5);
          }
        }
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community template cache metadata {}: {}", path.string(), e.what());
      }
      return metadata;
    }

    void writeCacheMetadata(const std::filesystem::path& dir, const CacheMetadata& metadata) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);

      nlohmann::json root;
      root["version"] = 1;
      root["files"] = nlohmann::json::object();
      for (const auto& [name, md5] : metadata.fileMd5s) {
        root["files"][name] = md5;
      }

      const std::filesystem::path path = cacheMetadataPath(dir);
      std::ofstream out(path);
      if (!out) {
        kLog.warn("failed to write community template cache metadata {}", path.string());
        return;
      }
      out << root.dump(2) << '\n';
    }

    void recordCachedFile(CacheMetadata& metadata, std::string_view name, const std::filesystem::path& path) {
      const std::string md5 = util::fileMd5Hex(path);
      if (!md5.empty()) {
        metadata.fileMd5s[std::string(name)] = md5;
      }
    }

    std::filesystem::perms fileModeMask() {
      using P = std::filesystem::perms;
      return P::owner_read
          | P::owner_write
          | P::owner_exec
          | P::group_read
          | P::group_write
          | P::group_exec
          | P::others_read
          | P::others_write
          | P::others_exec
          | P::set_uid
          | P::set_gid
          | P::sticky_bit;
    }

    void syncCachedFileMode(
        std::string_view templateId, const CommunityTemplateFile& file, const std::filesystem::path& dest
    ) {
      if (!file.mode.has_value())
        return;

      std::error_code ec;
      const std::filesystem::file_status status = std::filesystem::symlink_status(dest, ec);
      if (ec || !std::filesystem::exists(status))
        return;
      if (std::filesystem::is_symlink(status)) {
        kLog.warn("skipping permission sync for symlinked community template file {}", dest.string());
        return;
      }
      if (!std::filesystem::is_regular_file(status)) {
        kLog.warn("skipping permission sync for non-regular community template file {}", dest.string());
        return;
      }

      const std::filesystem::perms currentMode = status.permissions() & fileModeMask();
      if (currentMode == *file.mode)
        return;

      std::filesystem::permissions(dest, *file.mode, std::filesystem::perm_options::replace, ec);
      if (ec) {
        kLog.warn("failed to set mode for community template '{}' file '{}': {}", templateId, file.name, ec.message());
      }
    }

    bool cacheFileNeedsDownload(
        std::string_view templateId, const CommunityTemplateFile& file, const std::filesystem::path& dest,
        CacheMetadata& metadata
    ) {
      if (!std::filesystem::exists(dest)) {
        removeLegacyMd5Sidecar(dest);
        return true;
      }

      const std::string currentMd5 = util::fileMd5Hex(dest);
      if (currentMd5.empty()) {
        kLog.warn("failed to checksum cached community template file {}; preserving it", dest.string());
        return false;
      }

      const std::string catalogMd5 = StringUtils::toLower(file.md5);
      std::optional<std::string> recordedMd5;
      if (auto it = metadata.fileMd5s.find(file.name); it != metadata.fileMd5s.end()) {
        recordedMd5 = it->second;
      } else if (auto legacyMd5 = readLegacyMd5Sidecar(dest)) {
        if (*legacyMd5 == currentMd5) {
          metadata.fileMd5s[file.name] = currentMd5;
          recordedMd5 = currentMd5;
        } else {
          removeLegacyMd5Sidecar(dest);
          kLog.warn(
              "community template '{}' file '{}' has local edits; skipping upstream update", templateId, file.name
          );
          return false;
        }
      }
      removeLegacyMd5Sidecar(dest);

      if (!recordedMd5.has_value()) {
        if (!catalogMd5.empty() && currentMd5 == catalogMd5) {
          metadata.fileMd5s[file.name] = currentMd5;
        } else if (!catalogMd5.empty()) {
          kLog.warn(
              "community template '{}' file '{}' is untracked in the cache; skipping upstream update", templateId,
              file.name
          );
        }
        return false;
      }

      if (currentMd5 != *recordedMd5) {
        if (!catalogMd5.empty() && currentMd5 == catalogMd5) {
          metadata.fileMd5s[file.name] = currentMd5;
          return false;
        }
        kLog.warn("community template '{}' file '{}' has local edits; skipping upstream update", templateId, file.name);
        return false;
      }

      return !catalogMd5.empty() && currentMd5 != catalogMd5;
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

    std::string renderGeneratedTemplateToml(const CommunityTemplateInfo& info) {
      std::ostringstream out;

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
        if (!entry.postAction.empty())
          writeTomlString(out, "post_action", entry.postAction);
        if (entry.index.has_value())
          out << "index = " << *entry.index << "\n";
      }
      return out.str();
    }

    void syncGeneratedTemplateToml(const CommunityTemplateInfo& info, CacheMetadata& metadata) {
      if (info.entries.empty())
        return;

      constexpr std::string_view kTemplateToml = "template.toml";
      const std::filesystem::path path = communityTemplateConfigPath(info.id);
      const std::string desired = renderGeneratedTemplateToml(info);
      const bool exists = std::filesystem::exists(path);
      if (exists) {
        const std::string currentMd5 = util::fileMd5Hex(path);
        if (auto it = metadata.fileMd5s.find(std::string(kTemplateToml)); it != metadata.fileMd5s.end()) {
          if (currentMd5 != it->second) {
            const auto current = readTextFile(path);
            if (current.has_value() && *current == desired) {
              recordCachedFile(metadata, kTemplateToml, path);
              return;
            }
            kLog.warn("community template '{}' metadata has local edits; skipping catalog update", info.id);
            return;
          }
        } else {
          const auto current = readTextFile(path);
          if (!current.has_value() || *current != desired) {
            kLog.warn("community template '{}' metadata is untracked in the cache; skipping catalog update", info.id);
            return;
          }
        }
      }

      if (!writeTextFile(path, desired)) {
        kLog.warn("failed to write community template metadata {}", path.string());
        return;
      }
      recordCachedFile(metadata, kTemplateToml, path);
    }

    void appendTemplateOutputPaths(std::vector<std::string>& outputPaths, const toml::table& root) {
      const toml::table* templates = root["templates"].as_table();
      if (templates == nullptr)
        return;
      for (const auto& [idNode, node] : *templates) {
        const toml::table* tpl = node.as_table();
        if (tpl == nullptr)
          continue;
        const toml::node* op = tpl->get("output_path");
        if (op == nullptr)
          continue;
        if (const auto str = op->as_string()) {
          outputPaths.push_back(str->get());
        } else if (const auto arr = op->as_array()) {
          for (const auto& item : *arr) {
            if (const auto itemStr = item.as_string())
              outputPaths.push_back(itemStr->get());
          }
        }
      }
    }

    std::optional<AvailableTemplate> readTemplateTomlInfo(const std::filesystem::path& path, std::string_view cacheId) {
      if (!isSafeCommunityTemplateId(cacheId))
        return std::nullopt;

      try {
        toml::table root = toml::parse_file(path.string());
        const toml::table* catalog = root["catalog"].as_table();
        if (catalog == nullptr || catalog->empty())
          return std::nullopt;

        AvailableTemplate out;
        out.id = std::string(cacheId);
        out.displayName = out.id;

        const toml::table* info = catalog->get_as<toml::table>(cacheId);
        if (info == nullptr) {
          kLog.warn(
              "cached community template metadata {} does not contain catalog entry '{}'; using cache directory name",
              path.string(), cacheId
          );
          appendTemplateOutputPaths(out.outputPaths, root);
          return out;
        }

        {
          if (const auto name = info->get_as<std::string>("name"))
            out.displayName = name->get();
          if (const auto category = info->get_as<std::string>("category"))
            out.category = category->get();
        }
        appendTemplateOutputPaths(out.outputPaths, root);
        return out;
      } catch (const toml::parse_error&) {
        return std::nullopt;
      }
    }

    void appendOutputPathsFromCacheToml(AvailableTemplate& t) {
      const auto tomlPath = communityTemplateConfigPath(t.id);
      if (!std::filesystem::exists(tomlPath))
        return;
      try {
        const toml::table root = toml::parse_file(tomlPath.string());
        appendTemplateOutputPaths(t.outputPaths, root);
      } catch (const toml::parse_error&) {
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
    const std::filesystem::path cacheDir = communityTemplatesCacheDir();
    std::filesystem::create_directories(cacheDir, ec);
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
      auto metadata = std::make_shared<CacheMetadata>(readCacheMetadata(dir));
      if (!info->entries.empty()) {
        syncGeneratedTemplateToml(*info, *metadata);
      }

      for (const auto& file : info->files) {
        if (!isSafeRelativePath(file.name)) {
          kLog.warn("skipping unsafe community template path '{}' for '{}'", file.name, id);
          continue;
        }
        const std::filesystem::path dest = dir / std::filesystem::path(file.name);
        std::error_code existsEc;
        if (std::filesystem::exists(dest, existsEc)) {
          syncCachedFileMode(id, file, dest);
        }
        if (!cacheFileNeedsDownload(id, file, dest, *metadata))
          continue;
        if (dest.has_parent_path())
          std::filesystem::create_directories(dest.parent_path(), ec);
        ++(*pending);
        const std::string url =
            std::string(kCatalogUrl) + "/" + StringUtils::urlEncode(id) + "/" + urlEncodePath(file.name);
        m_httpClient.download(
            url, dest,
            [this, file, dest, dir, metadata, generation, pending, completed, notifyIfReady,
             templateId = std::string(id)](bool success) {
              ++(*completed);
              if (generation != m_generation)
                return;
              if (success) {
                removeLegacyMd5Sidecar(dest);
                const std::string actualMd5 = util::fileMd5Hex(dest);
                if (!file.md5.empty() && actualMd5 != StringUtils::toLower(file.md5)) {
                  std::error_code removeEc;
                  std::filesystem::remove(dest, removeEc);
                  metadata->fileMd5s.erase(file.name);
                  kLog.warn("downloaded community template file {} failed md5 validation", dest.string());
                } else if (!actualMd5.empty()) {
                  metadata->fileMd5s[file.name] = actualMd5;
                  syncCachedFileMode(templateId, file, dest);
                }
                writeCacheMetadata(dir, *metadata);
              } else {
                kLog.warn("failed to download community template file {}", dest.string());
              }
              notifyIfReady();
            }
        );
      }
      writeCacheMetadata(dir, *metadata);
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
      appendOutputPathsFromCacheToml(t);
      out.push_back(std::move(t));
    }

    const std::filesystem::path cacheDir = communityTemplatesCacheDir();
    std::error_code ec;
    if (!std::filesystem::is_directory(cacheDir, ec))
      return out;

    for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
      if (!entry.is_directory())
        continue;
      const std::string cacheId = entry.path().filename().string();
      const bool exists = std::ranges::contains(out, cacheId, &AvailableTemplate::id);
      if (exists)
        continue;
      const auto toml = entry.path() / "template.toml";
      if (!std::filesystem::exists(toml))
        continue;
      if (auto info = readTemplateTomlInfo(toml, cacheId)) {
        out.push_back(std::move(*info));
      }
    }

    std::ranges::sort(out, [](const AvailableTemplate& a, const AvailableTemplate& b) {
      if (a.category != b.category)
        return a.category < b.category;
      if (a.displayName != b.displayName)
        return a.displayName < b.displayName;
      return a.id < b.id;
    });
    return out;
  }

  // Stored under the state dir (not XDG cache): app-managed, re-fetchable data that
  // should persist between sessions and not be auto-reclaimed by cache cleaners.
  std::filesystem::path communityTemplatesCacheDir() {
    const std::string state = FileUtils::stateDir();
    if (!state.empty())
      return std::filesystem::path(state) / "community-templates";
    return std::filesystem::path("/tmp") / "noctalia" / "community-templates";
  }

  std::filesystem::path communityTemplateDir(std::string_view id) {
    return communityTemplatesCacheDir() / std::string(id);
  }

  std::filesystem::path communityTemplateConfigPath(std::string_view id) {
    return communityTemplateDir(id) / "template.toml";
  }

  bool isSafeCommunityTemplateId(std::string_view id) {
    return !id.empty() && std::ranges::all_of(id, [](unsigned char ch) {
      return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
  }

} // namespace noctalia::theme
