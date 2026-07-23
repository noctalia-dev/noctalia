#pragma once

#include "calendar/calendar_credential_store.h"
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
namespace security {
  class SecretStore;
}

// Background service that syncs configured online calendars (CalDAV directly, Google via the
// api.noctalia.dev OAuth broker) and exposes a merged, read-only event snapshot. Modeled on
// WeatherService: timer-driven via pollTimeoutMs()/tick(), durable credentials come from the
// account's explicit source, and a disk cache provides last-known-good data across restarts and
// network failures.
class CalendarService {
public:
  using ChangeCallback = std::function<void()>;
  enum class ConnectState : std::uint8_t { Idle, Pending, Success, Failed };
  enum class CredentialOperationResult : std::uint8_t {
    Success,
    MissingCredential,
    Unavailable,
    Cancelled,
    DeniedOrLocked,
    BackendError,
    FileError,
    ConfigError,
    CleanupError,
  };
  using ConfigMutation = std::function<bool()>;
  using CredentialOperationCallback = std::function<void(CredentialOperationResult)>;

  CalendarService(
      ConfigService& configService, HttpClient& httpClient, security::SecretStore& secretStore,
      NotificationManager* notifications = nullptr
  );

  void initialize();
  void addChangeCallback(ChangeCallback callback);
  void setCredentialChangeCallback(ChangeCallback callback) { m_credentialChangeCallback = std::move(callback); }

  [[nodiscard]] int pollTimeoutMs() const;
  void tick();

  [[nodiscard]] bool enabled() const noexcept { return m_activeConfig.enabled; }
  [[nodiscard]] bool hasData() const noexcept { return m_snapshot.valid; }
  [[nodiscard]] const CalendarSnapshot& snapshot() const noexcept { return m_snapshot; }

  // Start the Google OAuth Connect flow for a configured google account (opens a browser).
  void connectGoogleAccount(const std::string& accountId, const std::string& activationToken = {});
  void saveCalDavAccount(
      const std::string& accountId, CalendarCredentialSource credentialSource, const std::string& passwordFile,
      std::string password, ConfigMutation persistConfig, CredentialOperationCallback callback
  );
  void deleteAccount(const std::string& accountId, ConfigMutation removeConfig, CredentialOperationCallback callback);
  void retryCredentialMigration();
  // Schedule an immediate sync (used after saving CalDAV credentials).
  void requestRefresh();
  [[nodiscard]] ConnectState connectState() const noexcept { return m_connect.state; }
  [[nodiscard]] const std::string& connectingAccountId() const noexcept { return m_connect.accountId; }
  [[nodiscard]] calendar::CredentialState credentialState() const noexcept { return m_credentials.state(); }
  [[nodiscard]] bool credentialMigrationPending() const noexcept { return m_credentials.migrationPending(); }

private:
  struct ConnectFlow {
    ConnectState state = ConnectState::Idle;
    std::string accountId;
    std::string pollToken;
    bool inFlight = false;
    std::uint64_t generation = 0;
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
  void lookupCalDavPassword(
      const CalendarConfig::Account& account, calendar::CalendarCredentialStore::LookupCallback callback
  );
  void fetchGoogle(const CalendarConfig::Account& account);
  void refreshGoogleToken(const std::string& accountId, std::function<void(bool ok, std::string accessToken)> cb);
  void googleFetchWithToken(const std::string& accountId, const std::string& accessToken, bool allowRefreshRetry);
  void pollConnect();
  void notifyGoogleConnectFailure(const std::string& body) const;
  void initializeCredentials();
  [[nodiscard]] calendar::CredentialMigration credentialMigration();
  void storeGoogleTokens(
      const std::string& accountId, calendar::OAuthTokens tokens,
      std::function<void(security::SecretStoreStatus)> callback
  );
  void clearGoogleSession(const std::string& accountId);
  [[nodiscard]] static CredentialOperationResult operationResult(security::SecretStoreStatus status);

  void loadCache();
  void saveCache() const;
  [[nodiscard]] static std::filesystem::path cacheFilePath();

  ConfigService& m_configService;
  HttpClient& m_httpClient;
  NotificationManager* m_notifications = nullptr;
  CalendarConfig m_activeConfig;
  std::vector<ChangeCallback> m_callbacks;
  ChangeCallback m_credentialChangeCallback;

  calendar::GoogleOAuthBroker m_oauth;
  calendar::GoogleClient m_google;
  calendar::CalendarCredentialStore m_credentials;

  struct GoogleSession {
    std::string accessToken;
    std::chrono::system_clock::time_point expiry;
  };
  std::map<std::string, GoogleSession> m_googleSessions;

  CalendarSnapshot m_snapshot;
  std::map<std::string, std::vector<CalendarEvent>> m_eventsByAccount;
  std::chrono::steady_clock::time_point m_nextRefreshAt;
  bool m_refreshing = false;
  bool m_credentialsInitialized = false;
  std::size_t m_pendingAccounts = 0;
  ConnectFlow m_connect;
};
