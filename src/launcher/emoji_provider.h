#pragma once

#include "launcher/launcher_provider.h"

#include <string>
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

  void initialize() override;

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
  };

  std::vector<EmojiEntry> m_entries;
  ClipboardService* m_clipboard = nullptr;
};
