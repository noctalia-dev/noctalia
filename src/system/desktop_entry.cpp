#include "system/desktop_entry.h"

#include "core/inotify/inotify.h"
#include "core/log.h"
#include "i18n/language_tag.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("desktop_entry");

  bool isUserDesktopEntry(const fs::path& filepath) {
    fs::path dataHome;
    if (const char* configured = std::getenv("XDG_DATA_HOME"); configured != nullptr && configured[0] != '\0') {
      dataHome = configured;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      dataHome = fs::path(home) / ".local/share";
    } else {
      return false;
    }

    const fs::path applications = (dataHome / "applications").lexically_normal();
    const fs::path normalized = filepath.lexically_normal();
    const auto mismatch = std::ranges::mismatch(applications, normalized);
    return mismatch.in1 == applications.end();
  }

  fs::path resolveExecutable(std::string_view executable) {
    const fs::path path(executable);
    if (path.has_parent_path()) {
      return path;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return {};
    }

    const std::string_view searchPath(pathEnv);
    std::size_t start = 0;
    while (start <= searchPath.size()) {
      const std::size_t end = searchPath.find(':', start);
      const std::string_view directory =
          end == std::string_view::npos ? searchPath.substr(start) : searchPath.substr(start, end - start);
      const fs::path candidate = directory.empty() ? path : fs::path(directory) / path;
      std::error_code ec;
      if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate, ec)) {
        return candidate;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return {};
  }

  bool executableIsAppImage(std::string_view exec, bool resolveFromPath) {
    const auto start = exec.find_first_not_of(' ');
    if (start == std::string_view::npos) {
      return false;
    }

    const char quote = exec[start] == '"' ? '"' : '\0';
    const std::size_t executableStart = start + (quote == '\0' ? 0 : 1);
    const std::size_t executableEnd =
        quote == '\0' ? exec.find(' ', executableStart) : exec.find(quote, executableStart);
    const fs::path executable(exec.substr(
        executableStart,
        executableEnd == std::string_view::npos ? std::string_view::npos : executableEnd - executableStart
    ));
    if (StringUtils::toLower(executable.extension().string()) == ".appimage") {
      return true;
    }

    const fs::path resolved = executable.has_parent_path()
        ? executable
        : (resolveFromPath ? resolveExecutable(executable.string()) : fs::path{});
    std::ifstream file(resolved, std::ios::binary);
    std::array<char, 10> header{};
    if (!file.read(header.data(), static_cast<std::streamsize>(header.size()))) {
      return false;
    }
    // AppImage reserves these two ELF identification bytes for its format marker.
    return header[0] == '\x7F'
        && header[1] == 'E'
        && header[2] == 'L'
        && header[3] == 'F'
        && header[8] == 'A'
        && header[9] == 'I';
  }

  DesktopEntryOrigin detectOrigin(const fs::path& filepath, bool appImage) {
    const std::string path = filepath.lexically_normal().string();
    if (path.contains("/flatpak/") && path.contains("/exports/share/applications/")) {
      return DesktopEntryOrigin::Flatpak;
    }
    if (path.contains("/snap/") || path.contains("/snapd/desktop/applications/")) {
      return DesktopEntryOrigin::Snap;
    }
    if (path.contains("/nix/store/")) {
      return DesktopEntryOrigin::Nix;
    }
    if (appImage) {
      return DesktopEntryOrigin::AppImage;
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      const std::string userApplications = std::string(home) + "/.local/share/applications/";
      if (path.starts_with(userApplications)) {
        return DesktopEntryOrigin::User;
      }
    }
    return DesktopEntryOrigin::System;
  }

  bool parseDesktopBool(std::string_view value) {
    const std::string lower = StringUtils::toLower(value);
    return lower == "true" || lower == "1" || lower == "yes";
  }

  void splitMultipleDesktopStrings(std::vector<std::string>& parsedValues, std::string_view fullValue) {
    std::size_t start = 0;
    while (start < fullValue.size()) {
      const auto delimiter = fullValue.find(';', start);
      const auto token =
          (delimiter == std::string_view::npos) ? fullValue.substr(start) : fullValue.substr(start, delimiter - start);
      if (!token.empty()) {
        parsedValues.emplace_back(token);
      }
      if (delimiter == std::string_view::npos) {
        break;
      }
      start = delimiter + 1;
    }
  }

  bool
  shouldShowOnCurrentDesktop(const std::vector<std::string>& onlyShowIn, const std::vector<std::string>& notShowIn) {
    const auto visibleByDefault = onlyShowIn.empty();
    const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (currentDesktop == nullptr || currentDesktop[0] == '\0') {
      return visibleByDefault;
    }
    std::string_view desktops(currentDesktop);
    std::size_t start = 0;
    while (start <= desktops.size()) {
      const auto delimiter = desktops.find(':', start);
      const auto token =
          (delimiter == std::string_view::npos) ? desktops.substr(start) : desktops.substr(start, delimiter - start);
      if (!token.empty()) {
        if (std::ranges::find(onlyShowIn, token) != onlyShowIn.end()) {
          return true;
        } else if (std::ranges::find(notShowIn, token) != notShowIn.end()) {
          return false;
        }
      }
      if (delimiter == std::string_view::npos) {
        break;
      }
      start = delimiter + 1;
    }
    return visibleByDefault;
  }

  using LocalizedValues = std::unordered_map<std::string, std::string>;

  struct LocalizedAssignment {
    std::string_view key;
    std::string locale;
    std::string_view value;
  };

  std::string normalizeDesktopLocale(std::string_view rawLocale) {
    const auto modifierStart = rawLocale.find('@');
    const std::string_view base = rawLocale.substr(0, modifierStart);
    std::string locale = i18n::detail::normalizeLanguageTag(base);
    if (locale.empty() || modifierStart == std::string_view::npos) {
      return locale;
    }

    const std::string_view modifier = rawLocale.substr(modifierStart + 1);
    if (modifier.empty()) {
      return {};
    }
    locale += '@';
    for (const unsigned char character : modifier) {
      locale += static_cast<char>(std::tolower(character));
    }
    return locale;
  }

  std::optional<LocalizedAssignment> parseLocalizedAssignment(std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }

    const std::string_view fullKey = line.substr(0, equals);
    const auto bracket = fullKey.find('[');
    if (bracket == std::string_view::npos || bracket == 0 || fullKey.back() != ']') {
      return std::nullopt;
    }

    const std::string_view rawLocale = fullKey.substr(bracket + 1, fullKey.size() - bracket - 2);
    std::string locale = normalizeDesktopLocale(rawLocale);
    if (locale.empty()) {
      return std::nullopt;
    }

    return LocalizedAssignment{
        .key = fullKey.substr(0, bracket),
        .locale = std::move(locale),
        .value = line.substr(equals + 1),
    };
  }

  std::string localizedValue(std::string_view language, const LocalizedValues& values, std::string_view defaultValue) {
    for (const std::string& candidate : i18n::detail::catalogLanguageCandidates(language)) {
      if (const auto it = values.find(candidate); it != values.end()) {
        return it->second;
      }
    }
    return std::string(defaultValue);
  }

  void parseDesktopFile(const fs::path& filepath, std::string_view language, std::vector<DesktopEntry>& entries) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
      kLog.debug("failed to open desktop entry file '{}'", filepath.string());
      return;
    }

    DesktopEntry entry;
    entry.path = filepath.string();
    entry.id = filepath.stem().string();

    bool inDesktopEntry = false;
    bool inAction = false;
    LocalizedValues localizedNames;
    LocalizedValues localizedGenericNames;
    LocalizedValues localizedComments;
    LocalizedValues localizedKeywords;
    std::string type;
    bool hasAppImageMetadata = false;

    // Desktop-environment visibility lists (OnlyShowIn/NotShowIn)
    std::vector<std::string> onlyShowIn;
    std::vector<std::string> notShowIn;

    // Action parsing state
    std::vector<std::string> actionOrder;
    struct ActionData {
      std::string name;
      std::string exec;
      LocalizedValues localizedNames;
    };
    std::unordered_map<std::string, ActionData> actionMap;
    std::string currentActionId;
    ActionData currentActionData;

    auto flushCurrentAction = [&]() {
      if (!currentActionId.empty()) {
        currentActionData.name = localizedValue(language, currentActionData.localizedNames, currentActionData.name);
        if (!currentActionData.name.empty() && !currentActionData.exec.empty()) {
          actionMap[currentActionId] = currentActionData;
        }
        currentActionId.clear();
        currentActionData = {};
      }
    };

    std::string line;
    while (std::getline(file, line)) {
      // Strip trailing whitespace/carriage return
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }

      if (line.empty() || line[0] == '#') {
        continue;
      }

      if (line[0] == '[') {
        flushCurrentAction();
        inDesktopEntry = false;
        inAction = false;

        if (line == "[Desktop Entry]") {
          inDesktopEntry = true;
        } else if (line.size() > 17 && line.starts_with("[Desktop Action ") && line.back() == ']') {
          currentActionId = line.substr(16, line.size() - 17);
          if (!currentActionId.empty()) {
            inAction = true;
          }
        }
        continue;
      }

      if (inAction) {
        if (const auto localized = parseLocalizedAssignment(line); localized && localized->key == "Name") {
          currentActionData.localizedNames.insert_or_assign(localized->locale, localized->value);
          continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
          continue;
        std::string_view key(line.data(), eq);
        std::string_view value(line.data() + eq + 1, line.size() - eq - 1);
        if (key == "Name") {
          currentActionData.name = std::string(value);
        } else if (key == "Exec") {
          currentActionData.exec = std::string(value);
        }
        continue;
      }

      if (!inDesktopEntry) {
        continue;
      }

      if (const auto localized = parseLocalizedAssignment(line)) {
        LocalizedValues* values = nullptr;
        if (localized->key == "Name") {
          values = &localizedNames;
        } else if (localized->key == "GenericName") {
          values = &localizedGenericNames;
        } else if (localized->key == "Comment") {
          values = &localizedComments;
        } else if (localized->key == "Keywords") {
          values = &localizedKeywords;
        }
        if (values != nullptr) {
          values->insert_or_assign(localized->locale, localized->value);
        }
        continue;
      }

      auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }

      std::string_view key(line.data(), eq);
      std::string_view value(line.data() + eq + 1, line.size() - eq - 1);

      if (key == "Type") {
        type = std::string(value);
      } else if (key == "Name") {
        entry.name = std::string(value);
      } else if (key == "GenericName") {
        entry.genericName = std::string(value);
      } else if (key == "Comment") {
        entry.comment = std::string(value);
      } else if (key == "Exec") {
        entry.exec = std::string(value);
      } else if (key == "Icon") {
        entry.icon = std::string(value);
      } else if (key == "Categories") {
        entry.categories = std::string(value);
      } else if (key == "Keywords") {
        entry.keywords = std::string(value);
      } else if (key == "StartupWMClass") {
        entry.startupWmClass = std::string(value);
      } else if (key == "NoDisplay") {
        entry.noDisplay = parseDesktopBool(value);
      } else if (key == "Hidden") {
        entry.hidden = parseDesktopBool(value);
      } else if (key == "Path") {
        entry.workingDir = std::string(value);
      } else if (key == "Terminal") {
        entry.terminal = parseDesktopBool(value);
      } else if (key == "DBusActivatable") {
        entry.dbusActivatable = parseDesktopBool(value);
      } else if (key == "OnlyShowIn") {
        splitMultipleDesktopStrings(onlyShowIn, value);
      } else if (key == "NotShowIn") {
        splitMultipleDesktopStrings(notShowIn, value);
      } else if (key == "Actions") {
        splitMultipleDesktopStrings(actionOrder, value);
      } else if (key.starts_with("X-AppImage-")) {
        hasAppImageMetadata = true;
      }
    }

    // Flush any trailing action section.
    flushCurrentAction();

    if (type != "Application"
        || entry.noDisplay
        || entry.hidden
        || entry.name.empty()
        || !shouldShowOnCurrentDesktop(onlyShowIn, notShowIn)) {
      return;
    }

    const std::string defaultName = entry.name;
    entry.name = localizedValue(language, localizedNames, entry.name);
    entry.genericName = localizedValue(language, localizedGenericNames, entry.genericName);
    entry.comment = localizedValue(language, localizedComments, entry.comment);
    entry.keywords = localizedValue(language, localizedKeywords, entry.keywords);

    entry.localizedNamesLower.reserve(localizedNames.size() + 1);
    auto appendNameAlias = [&](std::string_view name) {
      const std::string lower = StringUtils::toLower(name);
      if (!lower.empty()
          && lower != StringUtils::toLower(entry.name)
          && !std::ranges::contains(entry.localizedNamesLower, lower)) {
        entry.localizedNamesLower.push_back(lower);
      }
    };
    appendNameAlias(defaultName);
    for (const auto& [_, name] : localizedNames) {
      appendNameAlias(name);
    }

    // Pre-lowercase for matching
    entry.nameLower = StringUtils::toLower(entry.name);
    entry.genericNameLower = StringUtils::toLower(entry.genericName);
    entry.keywordsLower = StringUtils::toLower(entry.keywords);
    entry.categoriesLower = StringUtils::toLower(entry.categories);
    entry.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
    entry.idLower = StringUtils::toLower(entry.id);
    entry.execLower = StringUtils::toLower(entry.exec);
    entry.origin =
        detectOrigin(filepath, hasAppImageMetadata || executableIsAppImage(entry.exec, isUserDesktopEntry(filepath)));

    // Build actions in the declared order.
    for (const auto& id : actionOrder) {
      auto it = actionMap.find(id);
      if (it != actionMap.end()) {
        entry.actions.push_back(
            DesktopAction{
                .id = it->first,
                .name = it->second.name,
                .exec = it->second.exec,
                .nameLower = StringUtils::toLower(it->second.name),
                .execLower = StringUtils::toLower(it->second.exec),
            }
        );
      }
    }

    entries.push_back(std::move(entry));
  }

  std::vector<std::string> xdgDataDirs() {
    std::vector<std::string> dirs;
    std::unordered_set<std::string> seen;

    auto appendDir = [&](std::string dir) {
      if (dir.empty()) {
        return;
      }
      if (seen.insert(dir).second) {
        dirs.push_back(std::move(dir));
      }
    };

    const char* home = std::getenv("XDG_DATA_HOME");
    if (home != nullptr && home[0] != '\0') {
      appendDir(home);
    } else {
      const char* userHome = std::getenv("HOME");
      if (userHome != nullptr) {
        appendDir(std::string(userHome) + "/.local/share");
      }
    }

    const char* dataDirs = std::getenv("XDG_DATA_DIRS");
    if (dataDirs != nullptr && dataDirs[0] != '\0') {
      std::string_view sv(dataDirs);
      std::size_t start = 0;
      while (start < sv.size()) {
        auto colon = sv.find(':', start);
        if (colon == std::string_view::npos) {
          appendDir(std::string(sv.substr(start)));
          break;
        }
        appendDir(std::string(sv.substr(start, colon - start)));
        start = colon + 1;
      }
    }

    // Keep canonical system directories as a safety net for partial env setups.
    appendDir("/usr/local/share");
    appendDir("/usr/share");

    return dirs;
  }

  class DesktopEntryCache {
  public:
    DesktopEntryCache() = default;

    ~DesktopEntryCache() { clearWatches(); }

    const std::vector<DesktopEntry>& entries() {
      refreshIfNeeded();
      return *m_entries;
    }

    // Worker-thread-safe shared snapshot. Deliberately non-refreshing:
    // freshness stays driven by the main thread's poll/reload path; this only
    // synchronizes against the reload swap.
    std::shared_ptr<const std::vector<DesktopEntry>> entriesSnapshot() const {
      std::scoped_lock lock(m_entriesMutex);
      return m_entries;
    }

    std::uint64_t version() {
      refreshIfNeeded();
      return m_version;
    }

    int watchFd() const noexcept { return m_inotify.fd(); }

    void checkSourcesChanged() {
      if (computeSourceSignature() != m_sourceSignature) {
        kLog.debug("desktop entry source signature changed; marking cache dirty");
        m_dirty = true;
      }
    }

    void checkReload() {
      if (m_inotify.fd() < 0) {
        return;
      }

      bool changed = false;
      m_inotify.drain([this, &changed](const inotify_event* event) {
        if ((event->mask & IN_IGNORED) != 0) {
          m_watches.erase(event->wd);
        } else {
          changed = true;
        }
      });

      if (changed) {
        kLog.debug("desktop entry inotify detected filesystem changes; marking cache dirty");
        m_dirty = true;
      }
    }

    void setLanguage(std::string_view language) {
      const std::string normalized = i18n::detail::normalizeLanguageTag(language);
      if (normalized == m_language) {
        return;
      }
      m_language = normalized;
      m_dirty = true;
    }

  private:
    void refreshIfNeeded() {
      if (!m_dirty) {
        return;
      }

      auto scanned = std::make_shared<const std::vector<DesktopEntry>>(scanDesktopEntries(m_language));
      {
        std::scoped_lock lock(m_entriesMutex);
        m_entries = std::move(scanned);
      }
      rebuildWatches();
      m_sourceSignature = computeSourceSignature();
      m_dirty = false;
      ++m_version;
      kLog.debug("refreshed desktop entries: {} apps (version {})", m_entries->size(), m_version);
    }

    // Signature of the resolved application source directories: canonical path
    // plus device/inode/mtime. The canonical path and inode change when a Nix
    // profile generation is swapped; the directory mtime changes when entries
    // are added or removed in place.
    std::string computeSourceSignature() const {
      std::string sig;
      const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
      sig += "xdg_current_desktop=";
      sig += (currentDesktop != nullptr && currentDesktop[0] != '\0') ? currentDesktop : "<unset>";
      sig += '\n';

      for (const auto& dataDir : xdgDataDirs()) {
        const fs::path appDir = fs::path(dataDir) / "applications";
        std::error_code ec;
        const fs::path resolved = fs::weakly_canonical(appDir, ec);
        const std::string path = ec ? appDir.string() : resolved.string();

        sig += path;
        struct ::stat st{};
        if (::stat(path.c_str(), &st) == 0) {
          sig += ':';
          sig += std::to_string(static_cast<unsigned long long>(st.st_dev));
          sig += ':';
          sig += std::to_string(static_cast<unsigned long long>(st.st_ino));
          sig += ':';
          sig += std::to_string(static_cast<long long>(st.st_mtim.tv_sec));
          sig += ':';
          sig += std::to_string(static_cast<long long>(st.st_mtim.tv_nsec));
        } else {
          sig += ":missing";
        }
        sig += '\n';
      }
      return sig;
    }

    void clearWatches() {
      if (m_inotify.fd() >= 0) {
        for (const auto& [wd, _] : m_watches)
          m_inotify.unwatch(wd);
      }
      m_watches.clear();
      m_watchedPaths.clear();
    }

    void rebuildWatches() {
      clearWatches();

      if (m_inotify.fd() < 0) {
        return;
      }

      for (const auto& dataDir : xdgDataDirs()) {
        const fs::path appDir = fs::path(dataDir) / "applications";
        std::error_code ec;
        if (!fs::is_directory(appDir, ec)) {
          continue;
        }

        addWatch(appDir);
        for (fs::recursive_directory_iterator it(appDir, ec), end; it != end; it.increment(ec)) {
          if (ec) {
            ec.clear();
            continue;
          }
          if (it->is_directory(ec) && !ec) {
            addWatch(it->path());
          }
        }
      }
    }

    void addWatch(const fs::path& path) {
      const auto key = path.string();
      if (!m_watchedPaths.insert(key).second) {
        return;
      }

      constexpr std::uint32_t kMask = IN_CREATE
          | IN_DELETE
          | IN_MOVED_FROM
          | IN_MOVED_TO
          | IN_CLOSE_WRITE
          | IN_DELETE_SELF
          | IN_MOVE_SELF
          | IN_ATTRIB;
      const auto wd = m_inotify.watch(key.c_str(), kMask);
      if (wd.has_value()) {
        m_watches[wd.value()] = key;
      } else {
        kLog.warn("failed to watch desktop entry directory '{}'", key);
      }
    }

    std::shared_ptr<const std::vector<DesktopEntry>> m_entries = std::make_shared<std::vector<DesktopEntry>>();
    mutable std::mutex m_entriesMutex; // guards the m_entries swap against entriesSnapshot() readers
    std::uint64_t m_version = 0;
    Inotify m_inotify;
    bool m_dirty = true;
    std::unordered_map<int, std::string> m_watches;
    std::unordered_set<std::string> m_watchedPaths;
    std::string m_sourceSignature;
    std::string m_language;
  };

  DesktopEntryCache& cache() {
    static DesktopEntryCache instance;
    return instance;
  }

} // namespace

std::vector<DesktopEntry> scanDesktopEntries(std::string_view language) {
  std::vector<DesktopEntry> entries;

  // Track seen IDs to deduplicate (first occurrence wins per XDG spec).
  // Hidden/NoDisplay files still claim their ID so user-local overrides can
  // suppress lower-priority system entries.
  std::unordered_set<std::string> seenIds;

  for (const auto& dataDir : xdgDataDirs()) {
    fs::path appDir = fs::path(dataDir) / "applications";
    std::error_code ec;
    if (!fs::is_directory(appDir, ec)) {
      continue;
    }

    constexpr auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(appDir, options, ec), end; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!it->is_regular_file(ec)) {
        ec.clear();
        continue;
      }
      if (it->path().extension() != ".desktop") {
        continue;
      }

      std::string id = it->path().stem().string();
      if (!seenIds.insert(id).second) {
        continue;
      }

      parseDesktopFile(it->path(), language, entries);
    }
  }

  // Sort by name for consistent ordering
  std::ranges::sort(entries, {}, &DesktopEntry::nameLower);

  return entries;
}

const std::vector<DesktopEntry>& desktopEntries() { return cache().entries(); }

std::shared_ptr<const std::vector<DesktopEntry>> desktopEntriesSnapshot() { return cache().entriesSnapshot(); }

std::uint64_t desktopEntriesVersion() { return cache().version(); }

void setDesktopEntryLanguage(std::string_view language) { cache().setLanguage(language); }

int desktopEntryWatchFd() noexcept { return cache().watchFd(); }

void checkDesktopEntryReload() { cache().checkReload(); }

void refreshDesktopEntriesIfSourcesChanged() { cache().checkSourcesChanged(); }
