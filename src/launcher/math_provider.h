#pragma once

#include "core/inotify/inotify.h"
#include "launcher/launcher_provider.h"

#include <filesystem>
#include <memory>
#include <optional>

class ClipboardService;
class ConfigService;
class HttpClient;
class Calculator;

class MathProvider : public LauncherProvider {
public:
  MathProvider(ClipboardService* clipboard, ConfigService* config, HttpClient* httpClient);
  ~MathProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "calc"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Calculator"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "calculator"; }
  [[nodiscard]] bool supportsAutoPaste() const override { return true; }

  void initialize() override;
  void onLauncherOpened() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  [[nodiscard]] std::vector<LauncherResult> evaluate(std::string_view text) const;

  // Replace m_calc with a fresh Calculator loaded with exchange rates and global + user
  // definitions, with unsafe functions disabled.
  void rebuildCalculator();

  // Start watching the user definitions directory if it exists. Returns whether a watch was added.
  bool watchDefinitionsDir();

  // Download fresh exchange rates over the async HTTP client, gated on
  // shell.launcher.fetch_exchange_rates and shell.offline_mode.
  void refreshExchangeRates();

  ClipboardService* m_clipboard = nullptr;
  ConfigService* m_config = nullptr;
  HttpClient* m_httpClient = nullptr;
  std::unique_ptr<Calculator> m_calc;
  std::filesystem::path m_definitionsDir;
  Inotify m_inotify;
  std::optional<int> m_definitionsWatcher;
};
