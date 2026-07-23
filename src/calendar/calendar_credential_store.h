#pragma once

#include "security/secret_store.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace calendar {

  enum class CredentialFileStatus {
    Success,
    NotFound,
    PermissionDenied,
    Invalid,
    IoError,
  };

  struct CredentialFileResult {
    CredentialFileStatus status = CredentialFileStatus::IoError;
    security::SecureBuffer value;
  };

  [[nodiscard]] CredentialFileResult readCredentialFile(const std::filesystem::path& path);

  enum class CredentialState {
    Opening,
    Ready,
    Unavailable,
    Cancelled,
    DeniedOrLocked,
    BackendError,
  };

  enum class CredentialKind {
    Password,
    RefreshToken,
  };

  struct LegacyCredential {
    std::string accountId;
    CredentialKind kind = CredentialKind::Password;
    security::SecureBuffer value;
  };

  struct CredentialMigration {
    bool complete = false;
    std::vector<LegacyCredential> credentials;
    std::function<bool()> finalize;
  };

  class CalendarCredentialStore {
  public:
    using Secret = std::shared_ptr<const security::SecureBuffer>;
    using LookupCallback = std::function<void(security::SecretStoreStatus, Secret)>;
    using StatusCallback = std::function<void(security::SecretStoreStatus)>;
    using ChangeCallback = std::function<void()>;

    explicit CalendarCredentialStore(security::SecretStore& secretStore);

    void initialize(CredentialMigration migration, StatusCallback callback = {});
    void retryMigration(CredentialMigration migration, StatusCallback callback = {});

    void lookupPassword(const std::string& accountId, LookupCallback callback);
    void lookupRefreshToken(const std::string& accountId, LookupCallback callback);
    void storePassword(const std::string& accountId, security::SecureBuffer value, StatusCallback callback);
    void storeRefreshToken(const std::string& accountId, security::SecureBuffer value, StatusCallback callback);
    void erasePassword(const std::string& accountId, StatusCallback callback);
    void eraseRefreshToken(const std::string& accountId, StatusCallback callback);
    void eraseAccount(const std::string& accountId, StatusCallback callback);
    void retainAccounts(const std::unordered_set<std::string>& accountIds);

    void setChangeCallback(ChangeCallback callback);
    [[nodiscard]] CredentialState state() const noexcept { return m_state; }
    [[nodiscard]] bool migrationPending() const noexcept { return m_migrationPending; }

  private:
    struct MigrationContext;

    static std::string_view secretName(CredentialKind kind);
    static std::string label(CredentialKind kind);
    static CredentialState stateForStatus(security::SecretStoreStatus status);

    void setState(CredentialState state, bool migrationPending);
    void runMigration(CredentialMigration migration, StatusCallback callback);
    void storeNextMigrationCredential(const std::shared_ptr<MigrationContext>& context);
    void lookup(CredentialKind kind, const std::string& accountId, LookupCallback callback);
    void
    store(CredentialKind kind, const std::string& accountId, security::SecureBuffer value, StatusCallback callback);
    void erase(CredentialKind kind, const std::string& accountId, StatusCallback callback);
    [[nodiscard]] security::SecretId secretId(CredentialKind kind, const std::string& accountId) const;

    security::SecretStore& m_secretStore;
    CredentialState m_state = CredentialState::Opening;
    bool m_migrationPending = false;
    ChangeCallback m_changeCallback;
    std::unordered_map<std::string, std::shared_ptr<security::SecureBuffer>> m_passwords;
    std::unordered_map<std::string, std::shared_ptr<security::SecureBuffer>> m_refreshTokens;
    std::unordered_set<std::string> m_missingPasswords;
    std::unordered_set<std::string> m_missingRefreshTokens;
  };

} // namespace calendar
