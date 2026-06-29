#pragma once

#include "calendar/calendar_types.h"
#include "calendar/google_client.h"
#include "calendar/google_oauth.h"
#include "config/config_types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

class ConfigService;
class HttpClient;
class NotificationManager;

// Background service that syncs configured online calendars (CalDAV directly, Google via the
// api.noctalia.dev OAuth broker) and exposes a merged, read-only event snapshot. Modeled on
// WeatherService: timer-driven via pollTimeoutMs()/tick(), credentials live in state.toml, and a
// disk cache provides last-known-good data across restarts and network failures.
class CalendarService {
public:
  using ChangeCallback = std::function<void()>;
  enum class ConnectState : std::uint8_t { Idle, Pending, Success, Failed };

  CalendarService(ConfigService& configService, HttpClient& httpClient, NotificationManager* notifications = nullptr);

  void initialize();
  void addChangeCallback(ChangeCallback callback);

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  [[nodiscard]] bool enabled() const noexcept { return m_activeConfig.enabled; }
  [[nodiscard]] bool hasData() const noexcept { return m_snapshot.valid; }
  [[nodiscard]] const CalendarSnapshot& snapshot() const noexcept { return m_snapshot; }

  // Start the Google OAuth Connect flow for a configured google account (opens a browser).
  void connectGoogleAccount(const std::string& accountId, const std::string& activationToken = {});
  // Schedule an immediate sync (used after saving CalDAV credentials).
  void requestRefresh();
  [[nodiscard]] ConnectState connectState() const noexcept { return m_connect.state; }
  [[nodiscard]] const std::string& connectingAccountId() const noexcept { return m_connect.accountId; }

private:
  struct ConnectFlow {
    ConnectState state = ConnectState::Idle;
    std::string accountId;
    std::string pollToken;
    bool inFlight = false;
    std::chrono::steady_clock::time_point deadline;
    std::chrono::steady_clock::time_point nextPollAt;
  };

  void onConfigReload();
  void notifyChanged();
  void startRefresh();
  void accountDone(const std::string& accountId, bool ok, std::vector<CalendarEvent> events);
  void rebuildSnapshot();
  void scheduleNextRefresh();

  void fetchCalDav(const CalendarConfig::Account& account);
  void fetchGoogle(const CalendarConfig::Account& account);
  void refreshGoogleToken(const std::string& accountId, std::function<void(bool ok, std::string accessToken)> cb);
  void googleFetchWithToken(const std::string& accountId, const std::string& accessToken, bool allowRefreshRetry);
  void pollConnect();
  void notifyGoogleConnectFailure(const std::string& body) const;

  // Credential helpers (state.toml, owner "calendar_credentials").
  [[nodiscard]] std::string credential(const std::string& accountId, const char* field) const;
  void setCredential(const std::string& accountId, const char* field, const std::string& value);
  void storeGoogleTokens(const std::string& accountId, const calendar::OAuthTokens& tokens);

  void loadCache();
  void saveCache() const;
  [[nodiscard]] static std::filesystem::path cacheFilePath();

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  NotificationManager* m_notifications = nullptr;
  CalendarConfig m_activeConfig;
  std::vector<ChangeCallback> m_callbacks;

  calendar::GoogleOAuthBroker m_oauth;
  calendar::GoogleClient m_google;

  CalendarSnapshot m_snapshot;
  std::map<std::string, std::vector<CalendarEvent>> m_eventsByAccount;
  std::chrono::steady_clock::time_point m_nextRefreshAt;
  bool m_refreshing = false;
  std::size_t m_pendingAccounts = 0;
  ConnectFlow m_connect;
};
