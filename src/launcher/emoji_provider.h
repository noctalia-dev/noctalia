#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ClipboardService;

class EmojiProvider : public LauncherProvider {
public:
  explicit EmojiProvider(ClipboardService* clipboard) : m_clipboard(clipboard) {}

  std::string_view defaultPrefix() const override { return "emo"; }
  std::string_view id() const override { return "Emoji"; }
  std::string displayName() const override;
  std::string_view defaultGlyphName() const override { return "mood-smile-beam"; }
  bool supportsGrid() const override { return true; }
  bool supportsAutoPaste() const override { return true; }
  bool trackUsage() const override { return true; }

  std::vector<LauncherCategory> categories() const override;
  std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  struct EmojiEntry {
    std::string emoji;
    std::string name;
    std::string nameLower;
    std::vector<std::string> keywords;
    std::string category;
    // Unicode codepoint for entries from unicode.json; 0 for emoji (no codepoint search).
    std::uint32_t codepoint = 0;
  };

  // Assets are parsed on the first query, not at startup, and stay resident afterwards.
  void ensureLoaded() const;
  void loadEntries(std::string_view assetName, bool unicode) const;

  mutable std::vector<EmojiEntry> m_entries;
  mutable bool m_loaded = false;
  ClipboardService* m_clipboard = nullptr;
};
