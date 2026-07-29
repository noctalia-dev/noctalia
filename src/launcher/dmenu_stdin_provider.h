#pragma once

#include "launcher/launcher_provider.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Ephemeral launcher provider for a stdin/dmenu session: candidates are pushed in
// (no command runs), and selection/cancel is reported through a completion callback.
// The launcher is opened scoped to a single instance (see LauncherPanel::setScopedProvider).
class DmenuStdinProvider : public LauncherProvider {
public:
  using Completion = std::function<void(std::optional<std::string> selection)>;

  // `lines` are raw candidate lines. `completion` fires exactly once: with the chosen
  // line on activate, or nullopt on cancel (panel closed without a selection).
  DmenuStdinProvider(std::vector<std::string> lines, std::string id, Completion completion);

  [[nodiscard]] std::string_view defaultPrefix() const override { return ""; }
  [[nodiscard]] std::string_view id() const override { return m_id; }
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "terminal"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;
  void reset() override;

private:
  struct Line {
    std::string raw;
    std::string title;
    std::string subtitle;
    std::string searchable;
  };

  std::vector<Line> m_lines;
  std::string m_id;
  Completion m_completion;
  bool m_completed = false;
};
