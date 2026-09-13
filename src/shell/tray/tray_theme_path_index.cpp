#include "shell/tray/tray_theme_path_index.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "shell/tray/tray_identifier.h"
#include "util/string_utils.h"

#include <cstdlib>
#include <ranges>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("tray");

  std::vector<std::string> xdgDataDirs() {
    std::vector<std::string> dataDirs;
    if (const char* env = std::getenv("XDG_DATA_HOME"); env != nullptr && env[0] != '\0') {
      dataDirs.emplace_back(env);
    } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      dataDirs.emplace_back(std::string(home) + "/.local/share");
    }
    const char* env = std::getenv("XDG_DATA_DIRS");
    const std::string dirs = (env != nullptr && env[0] != '\0') ? env : "/usr/local/share:/usr/share";
    for (const auto part : std::views::split(dirs, ':')) {
      if (std::string dir(part.begin(), part.end()); !dir.empty()) {
        dataDirs.push_back(std::move(dir));
      }
    }
    return dataDirs;
  }

} // namespace

namespace {

  // Canonical form catches symlinked twins; the lexical form still matches when
  // canonicalization fails (an unreadable parent, a dangling link). An empty
  // canonical form means "no canonical answer", never "matches everything".
  struct PathForms {
    fs::path canonical;
    fs::path lexical;
  };

  fs::path stripTrailingSlash(fs::path path) {
    path = path.lexically_normal();
    return path.has_filename() ? path : path.parent_path();
  }

  PathForms pathForms(const fs::path& path) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) {
      canonical.clear();
    } else {
      canonical = stripTrailingSlash(std::move(canonical));
    }
    return {.canonical = std::move(canonical), .lexical = stripTrailingSlash(path)};
  }

  bool sameRoot(const PathForms& a, const PathForms& b) {
    if (!a.canonical.empty() && !b.canonical.empty() && a.canonical == b.canonical) {
      return true;
    }
    if (a.lexical == b.lexical) {
      return true;
    }
    return (!a.canonical.empty() && a.canonical == b.lexical) || (!b.canonical.empty() && b.canonical == a.lexical);
  }

} // namespace

bool tray::isSystemIconRoot(const fs::path& path) {
  const PathForms probe = pathForms(path);

  for (const auto& dataDir : xdgDataDirs()) {
    for (const char* leaf : {"/icons", "/pixmaps"}) {
      if (sameRoot(probe, pathForms(fs::path(dataDir + leaf)))) {
        return true;
      }
    }
  }
  return false;
}

tray::ThemePathIndex tray::buildThemePathIndex(const fs::path& root) {
  ThemePathIndex index;

  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return index;
  }
  if (isSystemIconRoot(root)) {
    kLog.debug("ignoring tray icon theme path '{}': it is a system icon root", root.string());
    return index;
  }

  std::size_t scanned = 0;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (ec) {
      continue;
    }
    if (++scanned > kThemePathMaxEntries) {
      kLog.warn("tray icon theme path '{}' is too large to index; stopping the scan", root.string());
      break;
    }

    std::error_code dirEc;
    if (it->is_directory(dirEc) && !dirEc) {
      const std::string name = StringUtils::toLower(it->path().filename().string());
      if (it.depth() >= kThemePathMaxDepth || name.contains("cursor")) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file()) {
      continue;
    }

    const fs::path path = it->path();
    const auto extension = StringUtils::toLower(path.extension().string());
    if (extension != ".svg" && extension != ".png") {
      continue;
    }

    for (const auto& variant : identifierVariants(path.stem().string())) {
      index.try_emplace(variant, path.string());
    }
  }

  return index;
}

tray::ThemePathIconStore& tray::ThemePathIconStore::instance() {
  static ThemePathIconStore store;
  return store;
}

tray::ThemePathIconStore::ThemePathIconStore() : m_worker([this]() { workerLoop(); }) {}

tray::ThemePathIconStore::~ThemePathIconStore() {
  {
    std::scoped_lock lock(m_mutex);
    m_shutdown = true;
  }
  m_cv.notify_all();
  if (m_worker.joinable()) {
    m_worker.join();
  }
}

std::string tray::ThemePathIconStore::resolve(std::string_view themePath, std::string_view iconName) {
  if (themePath.empty() || iconName.empty()) {
    return {};
  }

  std::string key(themePath);
  {
    std::scoped_lock lock(m_mutex);
    if (const auto it = m_indexes.find(key); it != m_indexes.end()) {
      for (const auto& variant : identifierVariants(iconName)) {
        if (const auto found = it->second.find(variant); found != it->second.end()) {
          return found->second;
        }
      }
      return {};
    }
    if (!m_pending.insert(key).second) {
      return {};
    }
    m_queue.push_back(std::move(key));
  }
  m_cv.notify_one();
  return {};
}

std::uint64_t tray::ThemePathIconStore::addListener(std::function<void()> callback) {
  std::scoped_lock lock(m_mutex);
  const std::uint64_t id = m_nextListenerId++;
  m_listeners.emplace(id, std::move(callback));
  return id;
}

void tray::ThemePathIconStore::removeListener(std::uint64_t id) {
  std::scoped_lock lock(m_mutex);
  m_listeners.erase(id);
}

std::size_t tray::ThemePathIconStore::scansPerformed() const {
  std::scoped_lock lock(m_mutex);
  return m_scans;
}

void tray::ThemePathIconStore::notifyListeners() {
  std::vector<std::function<void()>> callbacks;
  {
    std::scoped_lock lock(m_mutex);
    callbacks.reserve(m_listeners.size());
    for (const auto& [id, callback] : m_listeners) {
      callbacks.push_back(callback);
    }
  }
  for (const auto& callback : callbacks) {
    callback();
  }
}

void tray::ThemePathIconStore::workerLoop() {
  while (true) {
    std::string path;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait(lock, [this]() { return m_shutdown || !m_queue.empty(); });
      if (m_shutdown) {
        return;
      }
      path = std::move(m_queue.back());
      m_queue.pop_back();
    }

    ThemePathIndex index = buildThemePathIndex(path);
    {
      std::scoped_lock lock(m_mutex);
      m_pending.erase(path);
      m_indexes.insert_or_assign(std::move(path), std::move(index));
      ++m_scans;
    }
    DeferredCall::callLater([]() { instance().notifyListeners(); });
  }
}
