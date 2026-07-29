#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileUtils {

  [[nodiscard]] constexpr std::filesystem::perms privateFileMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  [[nodiscard]] constexpr std::filesystem::perms privateDirectoryMode() {
    return privateFileMode() | std::filesystem::perms::owner_exec;
  }

  inline bool setPrivateFilePermissions(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::permissions(path, privateFileMode(), std::filesystem::perm_options::replace, ec);
    return !ec;
  }

  inline bool setPrivateDirectoryPermissions(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::permissions(path, privateDirectoryMode(), std::filesystem::perm_options::replace, ec);
    return !ec;
  }

  inline bool createPrivateDirectories(const std::filesystem::path& path, std::error_code& ec) {
    std::filesystem::create_directories(path, ec);
    if (ec) {
      return false;
    }
    return setPrivateDirectoryPermissions(path, ec);
  }

  [[nodiscard]] inline std::filesystem::path expandUserPath(const std::string& path) {
    if (path.empty() || path[0] != '~') {
      return std::filesystem::path(path);
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
      return std::filesystem::path(path);
    }
    if (path.size() == 1) {
      return std::filesystem::path(home);
    }
    if (path[1] == '/') {
      return std::filesystem::path(home) / path.substr(2);
    }
    return std::filesystem::path(path);
  }

  // Expands $NAME and ${NAME} environment-variable references. NAME matches
  // [A-Za-z_][A-Za-z0-9_]*; undefined variables expand to the empty string. A
  // lone '$' or '$' followed by a non-name character is left verbatim. This is a
  // deliberate, minimal substitution — never wordexp()/shell expansion, which
  // would run command substitution on config input.
  [[nodiscard]] inline std::string expandEnvVars(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    const auto isNameStart = [](char c) { return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '_'; };
    const auto isNameChar = [](char c) { return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_'; };

    for (std::size_t i = 0; i < in.size();) {
      if (in[i] != '$') {
        out.push_back(in[i]);
        ++i;
        continue;
      }
      // in[i] == '$'
      if (i + 1 < in.size() && in[i + 1] == '{') {
        const std::size_t close = in.find('}', i + 2);
        if (close != std::string_view::npos) {
          const std::string name(in.substr(i + 2, close - (i + 2)));
          if (const char* val = std::getenv(name.c_str()); val != nullptr) {
            out.append(val);
          }
          i = close + 1;
          continue;
        }
        // No closing brace — emit '$' verbatim and continue.
        out.push_back('$');
        ++i;
        continue;
      }
      if (i + 1 < in.size() && isNameStart(in[i + 1])) {
        std::size_t j = i + 1;
        while (j < in.size() && isNameChar(in[j])) {
          ++j;
        }
        const std::string name(in.substr(i + 1, j - (i + 1)));
        if (const char* val = std::getenv(name.c_str()); val != nullptr) {
          out.append(val);
        }
        i = j;
        continue;
      }
      // Lone '$' — leave verbatim.
      out.push_back('$');
      ++i;
    }
    return out;
  }

  // Expand XDG base-directory tokens ($XDG_CONFIG_HOME, etc.) to absolute paths
  // using the same spec-aware resolution as the template engine.
  [[nodiscard]] inline std::string expandXdgBaseDir(const std::string& path) {
    struct XdgBase {
      std::string_view token;
      std::string_view envVar;
      std::string_view homeDefault;
    };
    static constexpr std::array<XdgBase, 4> kBases = {{
        {"$XDG_CONFIG_HOME", "XDG_CONFIG_HOME", ".config"},
        {"$XDG_DATA_HOME", "XDG_DATA_HOME", ".local/share"},
        {"$XDG_STATE_HOME", "XDG_STATE_HOME", ".local/state"},
        {"$XDG_CACHE_HOME", "XDG_CACHE_HOME", ".cache"},
    }};
    for (const XdgBase& b : kBases) {
      if (!path.starts_with(b.token))
        continue;
      if (path.size() != b.token.size() && path[b.token.size()] != '/')
        continue;
      std::string base;
      if (const char* env = std::getenv(std::string(b.envVar).c_str()); env != nullptr && env[0] != '\0') {
        base = env;
      } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        base = std::string(home) + "/" + std::string(b.homeDefault);
      } else {
        return path;
      }
      return base + path.substr(b.token.size());
    }
    return path;
  }

  // Infer the client's config root from a template output path (e.g.
  // $XDG_CONFIG_HOME/vesktop/themes/file.css → $XDG_CONFIG_HOME/vesktop).
  // Returns nullopt when no recognisable client-root pattern is found.
  // This mirrors the logic in template_engine's shouldSkipTemplateOutput so
  // tooltips can filter to only paths that will actually be written.
  [[nodiscard]] inline std::optional<std::filesystem::path>
  inferClientConfigRoot(const std::filesystem::path& outputPath) {
    std::vector<std::filesystem::path> parts;
    parts.reserve(16);
    for (const auto& part : outputPath) {
      if (!part.empty() && part != std::filesystem::path("."))
        parts.push_back(part);
    }
    for (std::size_t i = 0; i + 3 < parts.size(); ++i) {
      if (parts[i] == ".var" && parts[i + 1] == "app" && parts[i + 3] == "config") {
        std::filesystem::path root;
        for (std::size_t j = 0; j <= i + 3; ++j)
          root /= parts[j];
        return root;
      }
    }
    std::filesystem::path current = outputPath.parent_path();
    while (!current.empty() && current != current.root_path()) {
      if (current.filename() == "themes") {
        std::filesystem::path parent = current.parent_path();
        const auto grandparent = parent.parent_path();
        if (parent.filename() == "extensions" || grandparent.filename() == "extensions") {
          std::filesystem::path walk = current;
          while (!walk.empty() && walk.filename() != "extensions")
            walk = walk.parent_path();
          if (walk.filename() == "extensions")
            return walk.parent_path();
        }
        return parent;
      }
      current = current.parent_path();
    }
    return std::nullopt;
  }

  // Returns true when the output path would actually be written by the template
  // engine (the client appears installed). Paths without a recognisable
  // client-root pattern are always treated as applicable.
  [[nodiscard]] inline bool isOutputPathApplicable(const std::string& outputPath) {
    const std::filesystem::path resolved = FileUtils::expandUserPath(expandXdgBaseDir(outputPath));
    if (auto root = inferClientConfigRoot(resolved)) {
      std::error_code ec;
      return std::filesystem::exists(*root, ec);
    }
    return true;
  }

  // Expand XDG base-directory tokens ($XDG_CONFIG_HOME, etc.) to their known
  // defaults under ~ for user-friendly display. Paths that already start with
  // $HOME are abbreviated to ~. Dynamic (script-resolved) paths are left as-is.
  [[nodiscard]] inline std::string xdgPathForDisplay(std::string_view raw) {
    struct Mapping {
      std::string_view token;
      std::string_view homeDefault;
    };
    static constexpr std::array<Mapping, 4> kMappings = {{
        {"$XDG_CONFIG_HOME", ".config"},
        {"$XDG_DATA_HOME", ".local/share"},
        {"$XDG_STATE_HOME", ".local/state"},
        {"$XDG_CACHE_HOME", ".cache"},
    }};
    std::string expanded(raw);
    for (const auto& m : kMappings) {
      if (!expanded.starts_with(m.token))
        continue;
      if (expanded.size() != m.token.size() && expanded[m.token.size()] != '/')
        continue;
      expanded = "~/" + std::string(m.homeDefault) + expanded.substr(m.token.size());
      break;
    }
    if (!expanded.starts_with("~/")) {
      const char* home = std::getenv("HOME");
      if (home != nullptr && home[0] != '\0') {
        const std::string homeStr(home);
        if (expanded.starts_with(homeStr) && (expanded.size() == homeStr.size() || expanded[homeStr.size()] == '/')) {
          expanded = "~" + expanded.substr(homeStr.size());
        }
      }
    }
    return expanded;
  }

  [[nodiscard]] inline bool
  containsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
    return std::ranges::contains(paths, path);
  }

  [[nodiscard]] inline std::optional<std::string> readSmallTextFile(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::string text;
    std::getline(file, text);
    if (text.empty()) {
      return std::nullopt;
    }

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
      text.pop_back();
    }
    return text;
  }

  [[nodiscard]] inline std::string configDir() {
    const char* noctalia = std::getenv("NOCTALIA_CONFIG_HOME");
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/noctalia";
    }
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/noctalia";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.config/noctalia";
    }
    return {};
  }

  [[nodiscard]] inline std::string stateDir() {
    const char* noctalia = std::getenv("NOCTALIA_STATE_HOME");
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/noctalia";
    }
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/noctalia";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/state/noctalia";
    }
    return {};
  }

  [[nodiscard]] inline std::string dataDir() {
    const char* noctalia = std::getenv("NOCTALIA_DATA_HOME");
    if (noctalia != nullptr && noctalia[0] != '\0') {
      return std::string(noctalia) + "/noctalia";
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
      return std::string(xdg) + "/noctalia";
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
      return std::string(home) + "/.local/share/noctalia";
    }
    return {};
  }

  // Git-source repo caches. Host-managed, re-fetchable, so they live under the
  // state dir — never config.
  [[nodiscard]] inline std::string pluginSourcesDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/sources";
  }

  // Exported runtime files for enabled git-source plugins. Re-derivable from
  // source repos; path sources and local dev plugins do not use this directory.
  [[nodiscard]] inline std::string pluginMaterializedDir() {
    const std::string base = stateDir();
    if (base.empty()) {
      return {};
    }
    return base + "/plugins/materialized";
  }

  // Persistent per-plugin data directory. Survives plugin updates (unlike the
  // materialized runtime dir) and honors the NOCTALIA_STATE_HOME override.
  // Caller is responsible for creating it. Empty if no state home resolves.
  [[nodiscard]] inline std::string pluginDataDir(const std::string& pluginId) {
    const std::string base = stateDir();
    if (base.empty() || pluginId.empty()) {
      return {};
    }
    return base + "/plugins/data/" + pluginId;
  }

  [[nodiscard]] inline std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    if (path.empty()) {
      return {};
    }

    std::error_code ec;
    const std::filesystem::path fsPath = expandUserPath(path);
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) {
      return {};
    }

    const std::uintmax_t fileSize = std::filesystem::file_size(fsPath, ec);
    if (ec || fileSize == 0) {
      return {};
    }

    constexpr std::uintmax_t kMaxBinaryReadBytes = 256ULL * 1024ULL * 1024ULL;
    if (fileSize > kMaxBinaryReadBytes) {
      return {};
    }

    std::ifstream file(fsPath, std::ios::binary);
    if (!file) {
      return {};
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));
    if (!file) {
      return {};
    }
    return data;
  }

  // Expands ~ and resolves relative paths against baseDir when provided; otherwise uses the
  // process working directory for relative paths.
  [[nodiscard]] inline std::filesystem::path
  resolvePath(std::string_view path, std::optional<std::string_view> baseDir = std::nullopt) {
    if (path.empty() || path.starts_with("color:")) {
      return std::filesystem::path(path);
    }

    std::filesystem::path resolved = expandUserPath(std::string(path));
    if (!resolved.is_absolute()) {
      if (baseDir.has_value() && !baseDir->empty()) {
        resolved = std::filesystem::path(*baseDir) / resolved;
      } else {
        std::error_code ec;
        resolved = std::filesystem::absolute(resolved, ec);
        if (ec) {
          return std::filesystem::path(path);
        }
      }
    }

    std::error_code ec;
    return resolved.lexically_normal();
  }

  [[nodiscard]] inline std::string normalizeWallpaperPath(std::string_view path) {
    if (path.empty() || path.starts_with("color:")) {
      return std::string(path);
    }

    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::path(path), ec);
    if (ec) {
      absolute = std::filesystem::path(path);
    }
    return expandUserPath(absolute.lexically_normal().string()).string();
  }

} // namespace FileUtils
