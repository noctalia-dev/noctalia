#include "calendar/calendar_service.h"

#include "calendar/caldav_client.h"
#include "calendar/caldav_discovery.h"
#include "calendar/calendar_cache.h"
#include "calendar/calendar_discovery_state.h"
#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "notification/notification_manager.h"
#include "security/secret_store.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <unordered_set>

namespace {
  constexpr Logger kLog("calendar");
  constexpr const char* kCredentialOwner = "calendar_credentials";
  constexpr const char* kSecurityMigrationOwner = "security_migrations";
  constexpr const char* kCalendarCredentialMigration = "calendar_credentials_v1";
  constexpr const char* kCalendarDiscoveryOwner = "calendar_discovery";
  constexpr const char* kICloudCalDavServerUrl = "https://caldav.icloud.com/";
  constexpr auto kConnectPollInterval = std::chrono::seconds{2};
  // Wide enough for month navigation in the control-center calendar (~1 year each way).
  constexpr auto kWindowBefore = std::chrono::hours{24 * 365};
  constexpr auto kWindowAfter = std::chrono::hours{24 * 365};

  std::int64_t toUnix(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
  }

  std::chrono::system_clock::time_point fromUnix(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
  }

  std::string caldavServerUrl(const CalendarConfig::Account& account) {
    if (account.provider == "icloud") {
      return kICloudCalDavServerUrl;
    }
    if (account.provider == "custom") {
      return account.serverUrl;
    }
    return {};
  }

  std::vector<CalendarSource> sourcesFromCollections(const std::vector<calendar::CalDavCollection>& collections) {
    std::vector<CalendarSource> sources;
    sources.reserve(collections.size());
    for (const calendar::CalDavCollection& collection : collections) {
      if (collection.id.empty()) {
        continue;
      }
      sources.push_back({
          .id = collection.id,
          .name = collection.name,
      });
    }
    return sources;
  }

  security::SecureBuffer secureBuffer(std::string_view value) {
    return security::SecureBuffer(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
  }

  std::string secretString(const calendar::CalendarCredentialStore::Secret& value) {
    if (!value) {
      return {};
    }
    const auto bytes = value->bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  void wipeString(std::string& value) {
    if (!value.empty()) {
      sodium_memzero(value.data(), value.size());
      value.clear();
    }
  }
} // namespace

CalendarService::CalendarService(
    ConfigService& configService, HttpClient& httpClient, security::SecretStore& secretStore,
    NotificationManager* notifications
)
    : m_configService(configService), m_httpClient(httpClient), m_notifications(notifications), m_oauth(httpClient),
      m_google(httpClient), m_credentials(secretStore) {}

void CalendarService::initialize() {
  m_activeConfig = m_configService.config().calendar;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  m_credentials.setChangeCallback([this]() {
    notifyChanged();
    if (m_credentialChangeCallback) {
      m_credentialChangeCallback();
    }
  });
  loadCache();
  initializeCredentials();
}

calendar::CredentialMigration CalendarService::credentialMigration() {
  calendar::CredentialMigration migration;
  migration.complete = m_configService.stateBool(kSecurityMigrationOwner, kCalendarCredentialMigration).value_or(false);
  if (migration.complete) {
    return migration;
  }

  for (const CalendarConfig::Account& account : m_activeConfig.accounts) {
    const auto append = [this, &migration, &account](std::string_view legacyName, calendar::CredentialKind kind) {
      const auto value = m_configService.stateString(kCredentialOwner, account.id + std::string(legacyName));
      if (value.has_value() && !value->empty()) {
        migration.credentials.push_back({
            .accountId = account.id,
            .kind = kind,
            .value = secureBuffer(*value),
        });
      }
    };
    if (account.credentialSource == CalendarCredentialSource::SecretService) {
      append("_password", calendar::CredentialKind::Password);
    }
    append("_refresh_token", calendar::CredentialKind::RefreshToken);
  }

  migration.finalize = [this]() {
    if (!m_configService.clearStateOwner(kCredentialOwner)) {
      return false;
    }
    return m_configService.setStateBool(kSecurityMigrationOwner, kCalendarCredentialMigration, true);
  };
  return migration;
}

void CalendarService::initializeCredentials() {
  m_credentials.initialize(credentialMigration(), [this](security::SecretStoreStatus status) {
    m_credentialsInitialized = true;
    if (status != security::SecretStoreStatus::Success) {
      kLog.warn("calendar credential migration is pending");
    }
    if (m_activeConfig.enabled) {
      m_nextRefreshAt = std::chrono::steady_clock::now();
    }
    notifyChanged();
  });
}

void CalendarService::retryCredentialMigration() {
  m_credentials.retryMigration(credentialMigration(), [this](security::SecretStoreStatus status) {
    if (status == security::SecretStoreStatus::Success && m_activeConfig.enabled) {
      m_nextRefreshAt = std::chrono::steady_clock::now();
    }
    notifyChanged();
  });
}

void CalendarService::addChangeCallback(ChangeCallback callback) {
  if (callback) {
    m_callbacks.push_back(std::move(callback));
  }
}

void CalendarService::notifyChanged() {
  for (auto& callback : m_callbacks) {
    if (callback) {
      callback();
    }
  }
}

void CalendarService::notifyGoogleConnectFailure(const std::string& body) const {
  if (m_notifications == nullptr) {
    kLog.warn("google connect failure notification dropped: notification manager unavailable");
    return;
  }
  m_notifications->addInternal(
      i18n::tr("notifications.internal.calendar"),
      i18n::tr("notifications.internal.calendar-google-connect-failed-title"), body, Urgency::Critical,
      kDefaultNotificationTimeout * 2, std::string("noctalia-glyph:calendar-x")
  );
}

void CalendarService::onConfigReload() {
  const CalendarConfig& next = m_configService.config().calendar;
  if (next == m_activeConfig) {
    return;
  }
  m_activeConfig = next;
  std::unordered_set<std::string> activeAccountIds;
  activeAccountIds.reserve(m_activeConfig.accounts.size());
  for (const CalendarConfig::Account& account : m_activeConfig.accounts) {
    activeAccountIds.insert(account.id);
  }
  m_credentials.retainAccounts(activeAccountIds);
  for (auto it = m_googleSessions.begin(); it != m_googleSessions.end();) {
    if (activeAccountIds.contains(it->first)) {
      ++it;
      continue;
    }
    wipeString(it->second.accessToken);
    it = m_googleSessions.erase(it);
  }
  if (!m_activeConfig.enabled) {
    m_eventsByAccount.clear();
    m_snapshot = CalendarSnapshot{};
    notifyChanged();
    return;
  }
  // Re-sync soon after any account/config change.
  m_nextRefreshAt = std::chrono::steady_clock::now();
  notifyChanged();
}

int CalendarService::pollTimeoutMs() const {
  const auto now = std::chrono::steady_clock::now();

  int timeout = -1;
  const auto consider = [&](std::chrono::steady_clock::time_point when) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
    const int clamped = ms < 0 ? 0 : static_cast<int>(std::min<std::int64_t>(ms, 60000));
    timeout = timeout < 0 ? clamped : std::min(timeout, clamped);
  };

  if (m_connect.state == ConnectState::Pending && !m_connect.inFlight) {
    consider(m_connect.nextPollAt);
  }
  if (m_credentialsInitialized && m_activeConfig.enabled && !m_refreshing) {
    consider(m_nextRefreshAt);
  }
  return timeout;
}

void CalendarService::tick() {
  const auto now = std::chrono::steady_clock::now();

  if (m_connect.state == ConnectState::Pending && !m_connect.inFlight && now >= m_connect.nextPollAt) {
    if (now >= m_connect.deadline) {
      kLog.warn("google connect timed out for account {}", m_connect.accountId);
      notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-timeout"));
      m_connect.state = ConnectState::Failed;
      notifyChanged();
    } else {
      pollConnect();
    }
  }

  if (m_credentialsInitialized && m_activeConfig.enabled && !m_refreshing && now >= m_nextRefreshAt) {
    startRefresh();
  }
}

void CalendarService::scheduleNextRefresh() {
  const int minutes = std::max<std::int32_t>(1, m_activeConfig.refreshMinutes);
  m_nextRefreshAt = std::chrono::steady_clock::now() + std::chrono::minutes{minutes};
}

void CalendarService::requestRefresh() {
  if (!m_activeConfig.enabled) {
    return;
  }
  m_nextRefreshAt = std::chrono::steady_clock::now();
  notifyChanged();
}

void CalendarService::startRefresh() {
  if (m_activeConfig.accounts.empty()) {
    scheduleNextRefresh();
    return;
  }
  m_refreshing = true;
  m_pendingAccounts = m_activeConfig.accounts.size();
  for (const CalendarConfig::Account& account : m_activeConfig.accounts) {
    if (account.type == "caldav") {
      fetchCalDav(account);
    } else if (account.type == "google") {
      fetchGoogle(account);
    } else {
      kLog.warn("unknown calendar account type '{}' for id {}", account.type, account.id);
      accountDone(account.id, false, {});
    }
  }
}

void CalendarService::accountDone(const std::string& accountId, bool ok, std::vector<CalendarEvent> events) {
  if (ok) {
    m_eventsByAccount[accountId] = std::move(events);
  }
  if (m_pendingAccounts > 0) {
    --m_pendingAccounts;
  }
  if (m_pendingAccounts == 0) {
    m_refreshing = false;
    rebuildSnapshot();
    saveCache();
    scheduleNextRefresh();
    notifyChanged();
  }
}

void CalendarService::rebuildSnapshot() {
  // Drop cached events for accounts no longer configured.
  for (auto it = m_eventsByAccount.begin(); it != m_eventsByAccount.end();) {
    const bool stillConfigured =
        std::ranges::contains(m_activeConfig.accounts, it->first, &CalendarConfig::Account::id);
    it = stillConfigured ? std::next(it) : m_eventsByAccount.erase(it);
  }

  std::vector<CalendarEvent> merged;
  for (const auto& [accountId, events] : m_eventsByAccount) {
    merged.insert(merged.end(), events.begin(), events.end());
  }
  std::ranges::sort(merged, {}, &CalendarEvent::start);
  m_snapshot.events = std::move(merged);
  m_snapshot.valid = true;
}

void CalendarService::fetchCalDav(const CalendarConfig::Account& account) {
  const std::string serverUrl = caldavServerUrl(account);
  const std::string username = account.username;
  if (serverUrl.empty() || username.empty()) {
    kLog.warn("caldav account {} is missing server_url/username", account.id);
    accountDone(account.id, false, {});
    return;
  }

  lookupCalDavPassword(
      account,
      [this, account, serverUrl,
       username](security::SecretStoreStatus status, calendar::CalendarCredentialStore::Secret password) mutable {
        if (status != security::SecretStoreStatus::Success || !password || password->empty()) {
          if (status == security::SecretStoreStatus::NotFound) {
            kLog.warn("caldav account {} has no stored password", account.id);
          }
          accountDone(account.id, false, {});
          return;
        }

        const auto now = std::chrono::system_clock::now();
        const std::string accountId = account.id;
        const std::string accountColor = account.color;
        const std::vector<std::string> selectedCalendars = account.calendars;
        const bool allowRedirectAuth = account.provider == "icloud";

        calendar::discoverCalDavCollections(
            m_httpClient, serverUrl, username, password, allowRedirectAuth,
            [this, accountId, username, password = std::move(password), accountColor, selectedCalendars,
             allowRedirectAuth, now](bool discovered, std::vector<calendar::CalDavCollection> collections) {
              if (!discovered) {
                accountDone(accountId, false, {});
                return;
              }

              const std::vector<CalendarSource> discoveredSources = sourcesFromCollections(collections);
              (void)m_configService.setStateString(
                  kCalendarDiscoveryOwner, accountId + "_calendars",
                  calendar::serializeCalendarSources(discoveredSources)
              );

              const std::vector<std::string> selectedIds =
                  calendar::selectedCalendarSourceIds(discoveredSources, selectedCalendars);
              std::erase_if(collections, [&](const calendar::CalDavCollection& collection) {
                return !std::ranges::contains(selectedIds, collection.id);
              });

              if (collections.empty()) {
                kLog.warn("caldav account {} has no selected calendars after discovery", accountId);
                accountDone(accountId, false, {});
                return;
              }

              struct FetchContext {
                CalendarService* service = nullptr;
                std::string accountId;
                std::size_t pending = 0;
                bool anyOk = false;
                std::vector<CalendarEvent> events;
              };
              auto ctx = std::make_shared<FetchContext>();
              ctx->service = this;
              ctx->accountId = accountId;
              ctx->pending = collections.size();

              for (const calendar::CalDavCollection& collection : collections) {
                calendar::CalDavAccount caldav;
                caldav.url = collection.url;
                caldav.username = username;
                caldav.password = password;
                caldav.calendarName = collection.name;
                caldav.color = accountColor.empty() ? collection.color : accountColor;

                calendar::fetchCalDavEvents(
                    m_httpClient, caldav, now - kWindowBefore, now + kWindowAfter, allowRedirectAuth,
                    [ctx](bool ok, std::vector<CalendarEvent> events) {
                      if (ok) {
                        ctx->anyOk = true;
                        ctx->events.insert(
                            ctx->events.end(), std::make_move_iterator(events.begin()),
                            std::make_move_iterator(events.end())
                        );
                      }
                      if (ctx->pending > 0) {
                        --ctx->pending;
                      }
                      if (ctx->pending == 0) {
                        ctx->service->accountDone(ctx->accountId, ctx->anyOk, std::move(ctx->events));
                      }
                    }
                );
              }
            }
        );
      }
  );
}

void CalendarService::lookupCalDavPassword(
    const CalendarConfig::Account& account, calendar::CalendarCredentialStore::LookupCallback callback
) {
  if (account.credentialSource == CalendarCredentialSource::SecretService) {
    m_credentials.lookupPassword(account.id, std::move(callback));
    return;
  }

  calendar::CredentialFileResult result = calendar::readCredentialFile(account.passwordFile);
  if (result.status == calendar::CredentialFileStatus::Success) {
    callback(security::SecretStoreStatus::Success, std::make_shared<security::SecureBuffer>(std::move(result.value)));
    return;
  }

  switch (result.status) {
  case calendar::CredentialFileStatus::NotFound:
    kLog.warn("caldav account {} password_file does not exist", account.id);
    callback(security::SecretStoreStatus::NotFound, {});
    return;
  case calendar::CredentialFileStatus::PermissionDenied:
    kLog.warn("caldav account {} password_file is not readable", account.id);
    callback(security::SecretStoreStatus::DeniedOrLocked, {});
    return;
  case calendar::CredentialFileStatus::Invalid:
    kLog.warn("caldav account {} password_file is empty, invalid, or too large", account.id);
    callback(security::SecretStoreStatus::BackendError, {});
    return;
  case calendar::CredentialFileStatus::IoError:
    kLog.warn("caldav account {} password_file could not be read", account.id);
    callback(security::SecretStoreStatus::BackendError, {});
    return;
  case calendar::CredentialFileStatus::Success:
    break;
  }
  callback(security::SecretStoreStatus::BackendError, {});
}

void CalendarService::refreshGoogleToken(const std::string& accountId, std::function<void(bool, std::string)> cb) {
  m_credentials.lookupRefreshToken(
      accountId,
      [this, accountId, cb = std::move(cb)](
          security::SecretStoreStatus status, calendar::CalendarCredentialStore::Secret storedRefreshToken
      ) mutable {
        if (status != security::SecretStoreStatus::Success || !storedRefreshToken || storedRefreshToken->empty()) {
          cb(false, {});
          return;
        }
        std::string refreshToken = secretString(storedRefreshToken);
        m_oauth.refresh(
            refreshToken,
            [this, accountId, cb = std::move(cb)](bool ok, bool invalidGrant, calendar::OAuthTokens tokens) mutable {
              if (!ok) {
                if (invalidGrant) {
                  kLog.warn("google account {} refresh token rejected; reconnect required", accountId);
                  clearGoogleSession(accountId);
                  m_credentials.eraseRefreshToken(accountId, [](security::SecretStoreStatus) {});
                }
                cb(false, {});
                return;
              }
              storeGoogleTokens(
                  accountId, std::move(tokens),
                  [this, accountId, cb = std::move(cb)](security::SecretStoreStatus storeStatus) mutable {
                    if (storeStatus != security::SecretStoreStatus::Success) {
                      clearGoogleSession(accountId);
                      cb(false, {});
                      return;
                    }
                    cb(true, m_googleSessions[accountId].accessToken);
                  }
              );
            }
        );
        wipeString(refreshToken);
      }
  );
}

void CalendarService::googleFetchWithToken(
    const std::string& accountId, const std::string& accessToken, bool allowRefreshRetry
) {
  const auto now = std::chrono::system_clock::now();
  m_google.fetchEvents(
      accessToken, now - kWindowBefore, now + kWindowAfter,
      [this, accountId, allowRefreshRetry](bool ok, bool unauthorized, std::vector<CalendarEvent> events) {
        if (unauthorized && allowRefreshRetry) {
          refreshGoogleToken(accountId, [this, accountId](bool refreshed, std::string newToken) {
            if (!refreshed) {
              accountDone(accountId, false, {});
            } else {
              googleFetchWithToken(accountId, newToken, false);
            }
          });
          return;
        }
        accountDone(accountId, ok && !unauthorized, std::move(events));
      }
  );
}

void CalendarService::fetchGoogle(const CalendarConfig::Account& account) {
  const std::string id = account.id;
  const auto session = m_googleSessions.find(id);
  const bool valid = session != m_googleSessions.end()
      && !session->second.accessToken.empty()
      && session->second.expiry > std::chrono::system_clock::now() + std::chrono::seconds{60};
  if (valid) {
    googleFetchWithToken(id, session->second.accessToken, true);
  } else {
    refreshGoogleToken(id, [this, id](bool ok, std::string token) {
      if (!ok) {
        accountDone(id, false, {});
      } else {
        googleFetchWithToken(id, token, false);
      }
    });
  }
}

void CalendarService::connectGoogleAccount(const std::string& accountId, const std::string& activationToken) {
  const auto it = std::ranges::find_if(m_activeConfig.accounts, [&](const CalendarConfig::Account& a) {
    return a.id == accountId && a.type == "google";
  });
  if (it == m_activeConfig.accounts.end()) {
    kLog.warn("connectGoogleAccount: no google account with id {}", accountId);
    notifyGoogleConnectFailure(
        i18n::tr("notifications.internal.calendar-google-connect-missing-account", "account", accountId)
    );
    return;
  }

  m_connect.state = ConnectState::Pending;
  const std::uint64_t generation = ++m_connect.generation;
  m_connect.accountId = accountId;
  m_connect.inFlight = true;
  m_connect.pollToken.clear();
  notifyChanged();

  m_oauth.start([this, accountId, activationToken,
                 generation](bool ok, calendar::GoogleOAuthBroker::StartResult result) {
    if (m_connect.generation != generation) {
      return;
    }
    m_connect.inFlight = false;
    if (!ok) {
      kLog.warn("google oauth start failed for account {}", accountId);
      if (result.httpStatus == 429) {
        notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-rate-limited"));
      } else if (result.httpStatus > 0) {
        notifyGoogleConnectFailure(
            i18n::tr("notifications.internal.calendar-google-connect-start-http", "status", result.httpStatus)
        );
      } else {
        notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-start-failed"));
      }
      m_connect.state = ConnectState::Failed;
      notifyChanged();
      return;
    }
    m_connect.pollToken = result.pollToken;
    const int expiresIn = result.expiresIn > 0 ? result.expiresIn : 600;
    m_connect.deadline = std::chrono::steady_clock::now() + std::chrono::seconds{expiresIn};
    m_connect.nextPollAt = std::chrono::steady_clock::now() + kConnectPollInterval;
    if (!net::openInBrowser(result.authUrl, activationToken)) {
      kLog.warn("failed to open browser for google consent; url logged at debug");
      kLog.debug("google consent url: {}", result.authUrl);
      notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-browser-failed"));
    }
    notifyChanged();
  });
}

void CalendarService::saveCalDavAccount(
    const std::string& accountId, CalendarCredentialSource credentialSource, const std::string& passwordFile,
    std::string password, ConfigMutation persistConfig, CredentialOperationCallback callback
) {
  if (!persistConfig || !callback) {
    return;
  }

  if (credentialSource == CalendarCredentialSource::File) {
    wipeString(password);
    if (passwordFile.empty()
        || !std::filesystem::path(passwordFile).is_absolute()
        || calendar::readCredentialFile(passwordFile).status != calendar::CredentialFileStatus::Success) {
      callback(CredentialOperationResult::FileError);
      return;
    }
    callback(persistConfig() ? CredentialOperationResult::Success : CredentialOperationResult::ConfigError);
    return;
  }

  if (password.empty()) {
    m_credentials.lookupPassword(
        accountId,
        [persistConfig = std::move(persistConfig), callback = std::move(callback)](
            security::SecretStoreStatus status, calendar::CalendarCredentialStore::Secret passwordValue
        ) mutable {
          if (status != security::SecretStoreStatus::Success || !passwordValue || passwordValue->empty()) {
            callback(
                status == security::SecretStoreStatus::Success ? CredentialOperationResult::MissingCredential
                                                               : operationResult(status)
            );
            return;
          }
          callback(persistConfig() ? CredentialOperationResult::Success : CredentialOperationResult::ConfigError);
        }
    );
    return;
  }

  auto newPassword = std::make_shared<security::SecureBuffer>(secureBuffer(password));
  wipeString(password);
  m_credentials.lookupPassword(
      accountId,
      [this, accountId, newPassword = std::move(newPassword), persistConfig = std::move(persistConfig),
       callback = std::move(callback)](
          security::SecretStoreStatus lookupStatus, calendar::CalendarCredentialStore::Secret oldPassword
      ) mutable {
        if (lookupStatus != security::SecretStoreStatus::Success
            && lookupStatus != security::SecretStoreStatus::NotFound) {
          callback(operationResult(lookupStatus));
          return;
        }
        m_credentials.storePassword(
            accountId, std::move(*newPassword),
            [this, accountId, oldPassword = std::move(oldPassword), persistConfig = std::move(persistConfig),
             callback = std::move(callback)](security::SecretStoreStatus storeStatus) mutable {
              if (storeStatus != security::SecretStoreStatus::Success) {
                callback(operationResult(storeStatus));
                return;
              }
              if (persistConfig()) {
                callback(CredentialOperationResult::Success);
                return;
              }

              const auto cleanupDone = [callback = std::move(callback)](security::SecretStoreStatus cleanupStatus) {
                const bool cleaned = cleanupStatus == security::SecretStoreStatus::Success
                    || cleanupStatus == security::SecretStoreStatus::NotFound;
                callback(cleaned ? CredentialOperationResult::ConfigError : CredentialOperationResult::CleanupError);
              };
              if (oldPassword) {
                m_credentials.storePassword(
                    accountId, security::SecureBuffer(oldPassword->bytes()), std::move(cleanupDone)
                );
              } else {
                m_credentials.erasePassword(accountId, std::move(cleanupDone));
              }
            }
        );
      }
  );
}

void CalendarService::deleteAccount(
    const std::string& accountId, ConfigMutation removeConfig, CredentialOperationCallback callback
) {
  if (!removeConfig || !callback) {
    return;
  }
  const auto account = std::ranges::find(m_activeConfig.accounts, accountId, &CalendarConfig::Account::id);
  if (account == m_activeConfig.accounts.end()) {
    callback(CredentialOperationResult::ConfigError);
    return;
  }
  if (account->type == "caldav" && account->credentialSource == CalendarCredentialSource::File) {
    clearGoogleSession(accountId);
    callback(removeConfig() ? CredentialOperationResult::Success : CredentialOperationResult::ConfigError);
    return;
  }
  m_credentials.eraseAccount(
      accountId,
      [this, accountId, removeConfig = std::move(removeConfig),
       callback = std::move(callback)](security::SecretStoreStatus status) mutable {
        if (status != security::SecretStoreStatus::Success && status != security::SecretStoreStatus::NotFound) {
          callback(operationResult(status));
          return;
        }
        clearGoogleSession(accountId);
        callback(removeConfig() ? CredentialOperationResult::Success : CredentialOperationResult::ConfigError);
      }
  );
}

void CalendarService::pollConnect() {
  m_connect.inFlight = true;
  const std::string accountId = m_connect.accountId;
  const std::uint64_t generation = m_connect.generation;
  m_oauth.poll(
      m_connect.pollToken,
      [this, accountId, generation](calendar::GoogleOAuthBroker::PollStatus status, calendar::OAuthTokens tokens) {
        if (m_connect.generation != generation) {
          return;
        }
        m_connect.inFlight = false;
        if (m_connect.accountId != accountId) {
          return; // a newer flow superseded this one
        }
        using PollStatus = calendar::GoogleOAuthBroker::PollStatus;
        switch (status) {
        case PollStatus::Pending:
          m_connect.nextPollAt = std::chrono::steady_clock::now() + kConnectPollInterval;
          break;
        case PollStatus::Complete:
          if (tokens.refreshToken.empty()) {
            notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-result-failed"));
            m_connect.state = ConnectState::Failed;
            notifyChanged();
            break;
          }
          m_connect.inFlight = true;
          storeGoogleTokens(
              accountId, std::move(tokens), [this, accountId, generation](security::SecretStoreStatus storeStatus) {
                if (m_connect.generation != generation) {
                  return;
                }
                m_connect.inFlight = false;
                if (m_connect.accountId != accountId) {
                  return;
                }
                if (storeStatus != security::SecretStoreStatus::Success) {
                  clearGoogleSession(accountId);
                  notifyGoogleConnectFailure(
                      i18n::tr("notifications.internal.calendar-google-connect-credential-store-failed")
                  );
                  m_connect.state = ConnectState::Failed;
                  notifyChanged();
                  return;
                }
                m_connect.state = ConnectState::Success;
                m_nextRefreshAt = std::chrono::steady_clock::now();
                notifyChanged();
              }
          );
          break;
        case PollStatus::Expired:
          notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-expired"));
          m_connect.state = ConnectState::Failed;
          notifyChanged();
          break;
        case PollStatus::Error:
          notifyGoogleConnectFailure(i18n::tr("notifications.internal.calendar-google-connect-result-failed"));
          m_connect.state = ConnectState::Failed;
          notifyChanged();
          break;
        }
      }
  );
}

void CalendarService::storeGoogleTokens(
    const std::string& accountId, calendar::OAuthTokens tokens,
    std::function<void(security::SecretStoreStatus)> callback
) {
  const bool hasRefreshToken = !tokens.refreshToken.empty();
  security::SecureBuffer refreshToken;
  if (hasRefreshToken) {
    refreshToken = secureBuffer(tokens.refreshToken);
    wipeString(tokens.refreshToken);
  }
  auto retainAccessToken = [this, accountId, tokens = std::move(tokens),
                            callback = std::move(callback)](security::SecretStoreStatus status) mutable {
    if (status == security::SecretStoreStatus::Success) {
      clearGoogleSession(accountId);
      m_googleSessions.insert_or_assign(
          accountId, GoogleSession{.accessToken = std::move(tokens.accessToken), .expiry = tokens.expiry}
      );
    }
    if (callback) {
      callback(status);
    }
  };

  if (!hasRefreshToken) {
    retainAccessToken(security::SecretStoreStatus::Success);
    return;
  }
  m_credentials.storeRefreshToken(accountId, std::move(refreshToken), std::move(retainAccessToken));
}

void CalendarService::clearGoogleSession(const std::string& accountId) {
  const auto it = m_googleSessions.find(accountId);
  if (it == m_googleSessions.end()) {
    return;
  }
  wipeString(it->second.accessToken);
  m_googleSessions.erase(it);
}

CalendarService::CredentialOperationResult CalendarService::operationResult(security::SecretStoreStatus status) {
  switch (status) {
  case security::SecretStoreStatus::Success:
    return CredentialOperationResult::Success;
  case security::SecretStoreStatus::NotFound:
    return CredentialOperationResult::MissingCredential;
  case security::SecretStoreStatus::Unavailable:
    return CredentialOperationResult::Unavailable;
  case security::SecretStoreStatus::Cancelled:
    return CredentialOperationResult::Cancelled;
  case security::SecretStoreStatus::DeniedOrLocked:
    return CredentialOperationResult::DeniedOrLocked;
  case security::SecretStoreStatus::BackendError:
    return CredentialOperationResult::BackendError;
  }
  return CredentialOperationResult::BackendError;
}

std::filesystem::path CalendarService::cacheFilePath() {
  const char* xdgCache = std::getenv("XDG_CACHE_HOME");
  std::filesystem::path base;
  if (xdgCache != nullptr && xdgCache[0] != '\0') {
    base = xdgCache;
  } else if (const char* home = std::getenv("HOME"); home != nullptr) {
    base = std::filesystem::path(home) / ".cache";
  } else {
    base = "/tmp";
  }
  return base / "noctalia" / "calendar" / "events.json";
}

void CalendarService::loadCache() {
  const std::filesystem::path path = cacheFilePath();
  if (!calendar::cache::secureExisting(path)) {
    kLog.warn("failed to secure calendar cache {}", path.string());
  }
  std::ifstream in(path);
  if (!in.is_open()) {
    return;
  }
  try {
    const auto j = nlohmann::json::parse(in);
    for (const auto& item : j.at("events")) {
      CalendarEvent event;
      event.id = item.value("id", std::string{});
      event.title = item.value("title", std::string{});
      event.calendarName = item.value("calendar", std::string{});
      event.colorHex = item.value("color", std::string{});
      event.location = item.value("location", std::string{});
      event.start = fromUnix(item.value("start", std::int64_t{0}));
      event.end = fromUnix(item.value("end", std::int64_t{0}));
      event.allDay = item.value("all_day", false);
      const std::string account = item.value("account", std::string{});
      m_eventsByAccount[account].push_back(std::move(event));
    }
    if (!m_eventsByAccount.empty()) {
      rebuildSnapshot();
    }
  } catch (const std::exception& e) {
    kLog.warn("failed to load calendar cache: {}", e.what());
    m_eventsByAccount.clear();
  }
}

void CalendarService::saveCache() const {
  const std::filesystem::path path = cacheFilePath();

  nlohmann::json events = nlohmann::json::array();
  for (const auto& [accountId, accountEvents] : m_eventsByAccount) {
    for (const CalendarEvent& event : accountEvents) {
      events.push_back({
          {"account", accountId},
          {"id", event.id},
          {"title", event.title},
          {"calendar", event.calendarName},
          {"color", event.colorHex},
          {"location", event.location},
          {"start", toUnix(event.start)},
          {"end", toUnix(event.end)},
          {"all_day", event.allDay},
      });
    }
  }

  if (!calendar::cache::write(path, nlohmann::json{{"events", std::move(events)}}.dump())) {
    kLog.warn("failed to write calendar cache {}", path.string());
  }
}
