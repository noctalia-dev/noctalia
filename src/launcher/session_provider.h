#pragma once

#include "launcher/launcher_provider.h"

class ConfigService;
class SessionActionRunner;

class SessionProvider : public LauncherProvider {
public:
  SessionProvider(ConfigService* config, SessionActionRunner* actionRunner);

  [[nodiscard]] std::string_view defaultPrefix() const override { return "session"; }
  [[nodiscard]] std::string_view id() const override { return "Session"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "power"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  ConfigService* m_config = nullptr;
  SessionActionRunner* m_actionRunner = nullptr;
};
