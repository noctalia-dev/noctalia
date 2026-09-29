#pragma once

#include "config/config_types.h"
#include "launcher/launcher_provider.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;

// One config-driven dmenu-style launcher entry. Runs `entry.command` once per open
// session, splits stdout into newline-separated candidates, and fuzzy-filters them.
// On activate: runs `entry.exec` (with {selection}/{query} substituted) or, when no
// exec is set, copies the selection to the clipboard.
//
// The listing command runs on a worker thread (never the main loop, which would
// deadlock IPC/DBus commands like `notify-send` or `noctalia msg`). Its stdout is
// parsed even on non-zero exit, so error messages still show up in the launcher.
class DmenuProvider : public LauncherProvider {
public:
  DmenuProvider(DmenuEntryConfig entry, ClipboardService* clipboard);
  ~DmenuProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return m_prefix; }
  [[nodiscard]] std::string_view id() const override { return m_id; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return m_glyph; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAutoPaste() const override { return !m_entry.exec.has_value(); }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return m_entry.global; }
  [[nodiscard]] bool isLoading() const override { return m_loading && !m_loaded; }

  void setResultsChangedCallback(std::function<void()> callback) override { m_onResultsChanged = std::move(callback); }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

  void reset() override;

private:
  struct Line {
    std::string raw;        // exact line; the selection value and result id
    std::string title;      // text before the first tab
    std::string subtitle;   // text after the first tab (empty if none)
    std::string searchable; // lowercased title + " " + subtitle
  };

  void ensureLoaded() const;
  static Line parseLine(std::string&& raw);
  static std::vector<Line> parseLines(std::string_view out);

  DmenuEntryConfig m_entry;
  std::string m_id;     // "dmenu." + entry.id
  std::string m_prefix; // entry.prefix value or empty
  std::string m_glyph;  // entry.glyph or "terminal"
  ClipboardService* m_clipboard = nullptr;
  std::function<void()> m_onResultsChanged;
  // False once the provider is destroyed, so pending callbacks can bail out.
  std::shared_ptr<std::atomic<bool>> m_alive;
  // Cancels an in-flight listing command (and its process group) on reset/destruction.
  mutable std::shared_ptr<std::atomic<bool>> m_cancel;
  mutable std::vector<Line> m_lines;
  mutable bool m_loaded = false;
  mutable bool m_loading = false;
  // Bumped on reset() so a stale run can't publish into a newer session.
  mutable std::uint64_t m_generation = 0;
};
