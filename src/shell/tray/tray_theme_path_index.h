#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tray {

  // App-private trees regularly nest a whole theme below the advertised root:
  // <root>/<theme>/<size>/<context>/<variant>/<name>.png puts the file at depth
  // 4, which a depth of 3 truncated. The extra headroom absorbs vendor layouts
  // with one or two more levels; the entry budget, not the depth, is what
  // bounds the worst case.
  inline constexpr int kThemePathMaxDepth = 6;
  inline constexpr std::size_t kThemePathMaxEntries = 20000;

  using ThemePathIndex = std::unordered_map<std::string, std::string>;

  // An SNI IconThemePath is meant to name an app-private icon directory. Some apps
  // publish a system icon root instead (or a theme inside one, such as
  // <datadir>/icons/hicolor), and indexing one of those walks every icon and cursor
  // theme installed on the machine. IconResolver already covers those roots properly
  // via index.theme and Inherits, so skip anything at or below one of them.
  [[nodiscard]] bool isSystemIconRoot(const std::filesystem::path& path);

  [[nodiscard]] ThemePathIndex buildThemePathIndex(const std::filesystem::path& root);

  // Process-wide index cache: every bar's TrayWidget shares one scan per path,
  // and the walk runs on a worker instead of the render thread. Every member
  // below is guarded by m_mutex; the worker owns the queue and the indexes,
  // while listeners are added, removed and fired on the main thread.
  class ThemePathIconStore {
  public:
    static ThemePathIconStore& instance();

    ThemePathIconStore(const ThemePathIconStore&) = delete;
    ThemePathIconStore& operator=(const ThemePathIconStore&) = delete;

    // Empty while the path is still being indexed; listeners fire once it lands.
    [[nodiscard]] std::string resolve(std::string_view themePath, std::string_view iconName);

    [[nodiscard]] std::uint64_t addListener(std::function<void()> callback);
    void removeListener(std::uint64_t id);

  private:
    ThemePathIconStore();
    ~ThemePathIconStore();

    void workerLoop();
    void notifyListeners();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, ThemePathIndex> m_indexes;
    std::unordered_set<std::string> m_pending;
    std::vector<std::string> m_queue;
    std::unordered_map<std::uint64_t, std::function<void()>> m_listeners;
    std::uint64_t m_nextListenerId = 1;
    bool m_shutdown = false;
    bool m_workerStarted = false;
    std::thread m_worker;
  };

} // namespace tray
