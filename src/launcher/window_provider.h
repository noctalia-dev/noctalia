#pragma once

#include "launcher/launcher_provider.h"

class CompositorPlatform;

// Lists currently open windows reported by the compositor and lets the user
// fuzzy-search them by title (or app id) and focus one. Activating a result
// asks the compositor to focus the window, switching workspace if needed.
class WindowProvider : public LauncherProvider {
public:
  explicit WindowProvider(CompositorPlatform* platform);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "win"; }
  [[nodiscard]] std::string_view id() const override { return "Windows"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "app-window"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  CompositorPlatform* m_platform = nullptr;
};
