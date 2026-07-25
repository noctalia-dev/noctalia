#pragma once

#include "config/config_types.h"
#include "launcher/launcher_provider.h"

class ClipboardService;

// One config-driven dmenu-style launcher entry. Runs `entry.command` once per open
// session, splits stdout into newline-separated candidates, and fuzzy-filters them.
// On activate: runs `entry.exec` (with {selection}/{query} substituted) or, when no
// exec is set, copies the selection to the clipboard.
class DmenuProvider : public LauncherProvider {
public:
  DmenuProvider(DmenuEntryConfig entry, ClipboardService* clipboard);

  [[nodiscard]] std::string_view defaultPrefix() const override { return m_prefix; }
  [[nodiscard]] std::string_view id() const override { return m_id; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return m_glyph; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAutoPaste() const override { return !m_entry.exec.has_value(); }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return m_entry.global; }

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

  DmenuEntryConfig m_entry;
  std::string m_id;     // "dmenu." + entry.id
  std::string m_prefix; // entry.prefix value or empty
  std::string m_glyph;  // entry.glyph or "terminal"
  ClipboardService* m_clipboard = nullptr;
  mutable std::vector<Line> m_lines;
  mutable bool m_loaded = false;
};
