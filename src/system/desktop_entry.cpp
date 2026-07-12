#include "system/desktop_entry.h"

#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <string_view>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("desktop_entry");

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

  struct LocaleInfo {
    std::string lang;
    std::string country;
  };

  LocaleInfo parseLocale() {
    LocaleInfo info;
    const char* lang = std::getenv("LANG");
    if (lang == nullptr) {
      lang = std::getenv("LC_MESSAGES");
    }
    if (lang == nullptr) {
      return info;
    }

    std::string_view sv(lang);

    // Strip encoding (e.g., ".UTF-8")
    auto dot = sv.find('.');
    if (dot != std::string_view::npos) {
      sv = sv.substr(0, dot);
    }

    // Strip modifier (e.g., "@euro")
    auto at = sv.find('@');
    if (at != std::string_view::npos) {
      sv = sv.substr(0, at);
    }

    auto underscore = sv.find('_');
    if (underscore != std::string_view::npos) {
      info.lang = std::string(sv.substr(0, underscore));
      info.country = std::string(sv);
    } else {
      info.lang = std::string(sv);
    }

    return info;
  }

  std::string extractLocalizedValue(const std::string& line, const std::string& key, const LocaleInfo& locale) {
    // Try key[lang_COUNTRY]=
    if (!locale.country.empty()) {
      std::string locKey = key + "[" + locale.country + "]=";
      if (line.size() > locKey.size() && line.starts_with(locKey)) {
        return line.substr(locKey.size());
      }
    }
    // Try key[lang]=
    if (!locale.lang.empty()) {
      std::string locKey = key + "[" + locale.lang + "]=";
      if (line.size() > locKey.size() && line.starts_with(locKey)) {
        return line.substr(locKey.size());
      }
    }
    return {};
  }

  void parseDesktopFile(const fs::path& filepath, std::vector<DesktopEntry>& entries) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
      return;
    }

    static const LocaleInfo locale = parseLocale();

    DesktopEntry entry;
    entry.path = filepath.string();
    entry.id = filepath.stem().string();

    bool inDesktopEntry = false;
    bool inAction = false;
    std::string localizedName;
    std::string localizedGenericName;
    std::string localizedComment;
    std::string type;

    // Desktop-environment visibility lists (OnlyShowIn/NotShowIn)
    std::vector<std::string> onlyShowIn;
    std::vector<std::string> notShowIn;

    // Action parsing state
    std::vector<std::string> actionOrder;
    struct ActionData {
      std::string name, exec, localizedName;
    };
    std::unordered_map<std::string, ActionData> actionMap;
    std::string currentActionId;
    ActionData currentActionData;

    auto flushCurrentAction = [&]() {
      if (!currentActionId.empty()) {
        if (!currentActionData.localizedName.empty()) {
          currentActionData.name = currentActionData.localizedName;
        }
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
        auto locName = extractLocalizedValue(line, "Name", locale);
        if (!locName.empty()) {
          currentActionData.localizedName = std::move(locName);
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

      // Check for localized values first
      auto locName = extractLocalizedValue(line, "Name", locale);
      if (!locName.empty()) {
        localizedName = std::move(locName);
        continue;
      }
      auto locGenericName = extractLocalizedValue(line, "GenericName", locale);
      if (!locGenericName.empty()) {
        localizedGenericName = std::move(locGenericName);
        continue;
      }
      auto locComment = extractLocalizedValue(line, "Comment", locale);
      if (!locComment.empty()) {
        localizedComment = std::move(locComment);
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
      } else if (key == "OnlyShowIn") {
        splitMultipleDesktopStrings(onlyShowIn, value);
      } else if (key == "NotShowIn") {
        splitMultipleDesktopStrings(notShowIn, value);
      } else if (key == "Actions") {
        splitMultipleDesktopStrings(actionOrder, value);
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

    // Apply localized values
    if (!localizedName.empty()) {
      entry.name = std::move(localizedName);
    }
    if (!localizedGenericName.empty()) {
      entry.genericName = std::move(localizedGenericName);
    }
    if (!localizedComment.empty()) {
      entry.comment = std::move(localizedComment);
    }

    // Pre-lowercase for matching
    entry.nameLower = StringUtils::toLower(entry.name);
    entry.genericNameLower = StringUtils::toLower(entry.genericName);
    entry.keywordsLower = StringUtils::toLower(entry.keywords);
    entry.categoriesLower = StringUtils::toLower(entry.categories);
    entry.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
    entry.idLower = StringUtils::toLower(entry.id);
    entry.execLower = StringUtils::toLower(entry.exec);

    // Build actions in the declared order.
    for (const auto& id : actionOrder) {
      auto it = actionMap.find(id);
      if (it != actionMap.end()) {
        entry.actions.push_back(
            DesktopAction{
                .id = it->first,
                .name = it->second.name,
                .exec = it->second.exec,
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
    DesktopEntryCache() { setupWatchFd(); }

    ~DesktopEntryCache() {
      clearWatches();
      if (m_inotifyFd >= 0) {
        ::close(m_inotifyFd);
      }
    }

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

    int watchFd() const noexcept { return m_inotifyFd; }

    void checkSourcesChanged() {
      if (computeSourceSignature() != m_sourceSignature) {
        m_dirty = true;
      }
    }

    void checkReload() {
      if (m_inotifyFd < 0) {
        return;
      }

      alignas(inotify_event) char buf[4096];
      bool changed = false;
      while (true) {
        const auto n = ::read(m_inotifyFd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }

        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(n)) {
          auto* event = reinterpret_cast<inotify_event*>(buf + offset);
          if ((event->mask & IN_IGNORED) != 0) {
            m_watches.erase(event->wd);
          } else {
            changed = true;
          }
          offset += sizeof(inotify_event) + event->len;
        }
      }

      if (changed) {
        m_dirty = true;
      }
    }

  private:
    void setupWatchFd() {
      m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (m_inotifyFd < 0) {
        kLog.warn("inotify_init1 failed, desktop entry hot reload disabled");
      }
    }

    void refreshIfNeeded() {
      if (!m_dirty) {
        return;
      }

      auto scanned = std::make_shared<const std::vector<DesktopEntry>>(scanDesktopEntries());
      {
        std::scoped_lock lock(m_entriesMutex);
        m_entries = std::move(scanned);
      }
      rebuildWatches();
      m_sourceSignature = computeSourceSignature();
      m_dirty = false;
      ++m_version;
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
      if (m_inotifyFd < 0) {
        m_watches.clear();
        m_watchedPaths.clear();
        return;
      }

      for (const auto& [wd, path] : m_watches) {
        (void)path;
        inotify_rm_watch(m_inotifyFd, wd);
      }
      m_watches.clear();
      m_watchedPaths.clear();
    }

    void rebuildWatches() {
      clearWatches();

      if (m_inotifyFd < 0) {
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
      const int wd = inotify_add_watch(m_inotifyFd, key.c_str(), kMask);
      if (wd < 0) {
        return;
      }
      m_watches[wd] = key;
    }

    std::shared_ptr<const std::vector<DesktopEntry>> m_entries = std::make_shared<std::vector<DesktopEntry>>();
    mutable std::mutex m_entriesMutex; // guards the m_entries swap against entriesSnapshot() readers
    std::uint64_t m_version = 0;
    int m_inotifyFd = -1;
    bool m_dirty = true;
    std::unordered_map<int, std::string> m_watches;
    std::unordered_set<std::string> m_watchedPaths;
    std::string m_sourceSignature;
  };

  DesktopEntryCache& cache() {
    static DesktopEntryCache instance;
    return instance;
  }

} // namespace

std::vector<DesktopEntry> scanDesktopEntries() {
  std::vector<DesktopEntry> entries;

  // Track seen IDs to deduplicate (first occurrence wins per XDG spec).
  // Hidden/NoDisplay files still claim their ID so user-local overrides can
  // suppress lower-priority system entries.
  std::unordered_set<std::string> seenIds;

  for (const auto& dataDir : xdgDataDirs()) {
    fs::path appDir = fs::path(dataDir) / "applications";
    if (!fs::is_directory(appDir)) {
      continue;
    }

    std::error_code ec;
    for (const auto& dirEntry : fs::recursive_directory_iterator(appDir, ec)) {
      if (!dirEntry.is_regular_file()) {
        continue;
      }
      if (dirEntry.path().extension() != ".desktop") {
        continue;
      }

      std::string id = dirEntry.path().stem().string();
      if (!seenIds.insert(id).second) {
        continue;
      }

      parseDesktopFile(dirEntry.path(), entries);
    }
  }

  // Sort by name for consistent ordering
  std::ranges::sort(entries, {}, &DesktopEntry::nameLower);

  return entries;
}

const std::vector<DesktopEntry>& desktopEntries() { return cache().entries(); }

std::shared_ptr<const std::vector<DesktopEntry>> desktopEntriesSnapshot() { return cache().entriesSnapshot(); }

std::uint64_t desktopEntriesVersion() { return cache().version(); }

int desktopEntryWatchFd() noexcept { return cache().watchFd(); }

void checkDesktopEntryReload() { cache().checkReload(); }

void refreshDesktopEntriesIfSourcesChanged() { cache().checkSourcesChanged(); }
