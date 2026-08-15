#include "calendar/calendar_credential_store.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace calendar {
  namespace {
    constexpr std::size_t kMaxCredentialFileSize = 64 * 1024;

    class FileDescriptor {
    public:
      explicit FileDescriptor(int fd) : m_fd(fd) {}
      ~FileDescriptor() {
        if (m_fd >= 0) {
          ::close(m_fd);
        }
      }

      FileDescriptor(const FileDescriptor&) = delete;
      FileDescriptor& operator=(const FileDescriptor&) = delete;

      [[nodiscard]] int get() const noexcept { return m_fd; }

    private:
      int m_fd = -1;
    };
  } // namespace

  CredentialFileResult readCredentialFile(const std::filesystem::path& path) {
    const int rawFd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (rawFd < 0) {
      if (errno == ENOENT || errno == ENOTDIR) {
        return {.status = CredentialFileStatus::NotFound};
      }
      if (errno == EACCES || errno == EPERM) {
        return {.status = CredentialFileStatus::PermissionDenied};
      }
      return {.status = CredentialFileStatus::IoError};
    }
    const FileDescriptor fd(rawFd);

    struct stat info{};
    if (::fstat(fd.get(), &info) != 0) {
      return {.status = CredentialFileStatus::IoError};
    }
    if (!S_ISREG(info.st_mode)
        || info.st_size <= 0
        || static_cast<std::uintmax_t>(info.st_size) > kMaxCredentialFileSize) {
      return {.status = CredentialFileStatus::Invalid};
    }

    const auto size = static_cast<std::size_t>(info.st_size);
    security::SecureBuffer value(size);
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t count = ::read(fd.get(), value.bytes().data() + offset, size - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      return {.status = CredentialFileStatus::IoError};
    }

    std::size_t trimmedSize = size;
    if (value.bytes()[trimmedSize - 1] == static_cast<std::uint8_t>('\n')) {
      --trimmedSize;
      if (trimmedSize > 0 && value.bytes()[trimmedSize - 1] == static_cast<std::uint8_t>('\r')) {
        --trimmedSize;
      }
    }
    const auto trimmed = value.bytes().first(trimmedSize);
    if (trimmed.empty() || std::ranges::contains(trimmed, static_cast<std::uint8_t>(0))) {
      return {.status = CredentialFileStatus::Invalid};
    }
    return {
        .status = CredentialFileStatus::Success,
        .value = security::SecureBuffer(trimmed),
    };
  }

  struct CalendarCredentialStore::MigrationContext {
    CredentialMigration migration;
    StatusCallback callback;
    std::size_t index = 0;
  };

  CalendarCredentialStore::CalendarCredentialStore(security::SecretStore& secretStore) : m_secretStore(secretStore) {}

  void CalendarCredentialStore::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  bool CalendarCredentialStore::refreshTokenMissing(const std::string& accountId) const {
    const auto it = m_refreshTokenStatuses.find(accountId);
    return it != m_refreshTokenStatuses.end() && it->second == security::SecretStoreStatus::NotFound;
  }

  bool CalendarCredentialStore::refreshTokenLocked(const std::string& accountId) const {
    const auto it = m_refreshTokenStatuses.find(accountId);
    return it != m_refreshTokenStatuses.end() && it->second == security::SecretStoreStatus::DeniedOrLocked;
  }

  bool CalendarCredentialStore::anyRefreshTokenMissing() const {
    return std::ranges::any_of(m_refreshTokenStatuses, [](const auto& item) {
      return item.second == security::SecretStoreStatus::NotFound;
    });
  }

  bool CalendarCredentialStore::anyRefreshTokenLocked() const {
    return std::ranges::any_of(m_refreshTokenStatuses, [](const auto& item) {
      return item.second == security::SecretStoreStatus::DeniedOrLocked;
    });
  }

  bool CalendarCredentialStore::updateRefreshTokenStatus(
      CredentialKind kind, const std::string& accountId, security::SecretStoreStatus status
  ) {
    if (kind != CredentialKind::RefreshToken) {
      return false;
    }
    const auto [it, inserted] = m_refreshTokenStatuses.try_emplace(accountId, status);
    if (!inserted && it->second == status) {
      return false;
    }
    it->second = status;
    return true;
  }

  std::string_view CalendarCredentialStore::secretName(CredentialKind kind) {
    switch (kind) {
    case CredentialKind::Password:
      return "password";
    case CredentialKind::RefreshToken:
      return "refresh-token";
    }
    return {};
  }

  std::string CalendarCredentialStore::label(CredentialKind kind) {
    switch (kind) {
    case CredentialKind::Password:
      return "Noctalia calendar password";
    case CredentialKind::RefreshToken:
      return "Noctalia calendar refresh token";
    }
    return "Noctalia calendar credential";
  }

  CredentialState CalendarCredentialStore::stateForStatus(security::SecretStoreStatus status) {
    switch (status) {
    case security::SecretStoreStatus::Success:
    case security::SecretStoreStatus::NotFound:
      return CredentialState::Ready;
    case security::SecretStoreStatus::Unavailable:
      return CredentialState::Unavailable;
    case security::SecretStoreStatus::Cancelled:
      return CredentialState::Cancelled;
    case security::SecretStoreStatus::DeniedOrLocked:
      return CredentialState::DeniedOrLocked;
    case security::SecretStoreStatus::BackendError:
      return CredentialState::BackendError;
    }
    return CredentialState::BackendError;
  }

  void CalendarCredentialStore::setState(CredentialState state, bool migrationPending, bool forceNotify) {
    if (!forceNotify && m_state == state && m_migrationPending == migrationPending) {
      return;
    }
    m_state = state;
    m_migrationPending = migrationPending;
    if (m_changeCallback) {
      m_changeCallback();
    }
  }

  void CalendarCredentialStore::initialize(CredentialMigration migration, StatusCallback callback) {
    runMigration(std::move(migration), std::move(callback));
  }

  void CalendarCredentialStore::retryMigration(CredentialMigration migration, StatusCallback callback) {
    const bool pending = !migration.complete;
    setState(CredentialState::Opening, pending);
    auto migrationState = std::make_shared<CredentialMigration>(std::move(migration));
    m_secretStore.retryAvailabilityCheck([this, migrationState = std::move(migrationState),
                                          callback = std::move(callback)](security::SecretStoreStatus status) mutable {
      if (status != security::SecretStoreStatus::Success) {
        setState(stateForStatus(status), !migrationState->complete);
        if (callback) {
          callback(status);
        }
        return;
      }
      runMigration(std::move(*migrationState), std::move(callback));
    });
  }

  void CalendarCredentialStore::runMigration(CredentialMigration migration, StatusCallback callback) {
    if (migration.complete) {
      setState(CredentialState::Ready, false);
      if (callback) {
        callback(security::SecretStoreStatus::Success);
      }
      return;
    }

    setState(CredentialState::Opening, true);
    auto context = std::make_shared<MigrationContext>();
    context->migration = std::move(migration);
    context->callback = std::move(callback);
    storeNextMigrationCredential(context);
  }

  void CalendarCredentialStore::storeNextMigrationCredential(const std::shared_ptr<MigrationContext>& context) {
    if (context->index >= context->migration.credentials.size()) {
      if (!context->migration.finalize || !context->migration.finalize()) {
        setState(CredentialState::BackendError, true);
        if (context->callback) {
          context->callback(security::SecretStoreStatus::BackendError);
        }
        return;
      }
      setState(CredentialState::Ready, false);
      if (context->callback) {
        context->callback(security::SecretStoreStatus::Success);
      }
      return;
    }

    LegacyCredential& credential = context->migration.credentials[context->index];
    const CredentialKind kind = credential.kind;
    const std::string accountId = credential.accountId;
    store(kind, accountId, std::move(credential.value), [this, context](security::SecretStoreStatus status) {
      if (status != security::SecretStoreStatus::Success) {
        setState(stateForStatus(status), true);
        if (context->callback) {
          context->callback(status);
        }
        return;
      }
      ++context->index;
      storeNextMigrationCredential(context);
    });
  }

  security::SecretId CalendarCredentialStore::secretId(CredentialKind kind, const std::string& accountId) const {
    return {
        .scope = "calendar",
        .owner = accountId,
        .name = std::string(secretName(kind)),
    };
  }

  void CalendarCredentialStore::lookupPassword(const std::string& accountId, LookupCallback callback) {
    lookup(CredentialKind::Password, accountId, std::move(callback));
  }

  void CalendarCredentialStore::lookupRefreshToken(const std::string& accountId, LookupCallback callback) {
    lookup(CredentialKind::RefreshToken, accountId, std::move(callback));
  }

  void CalendarCredentialStore::lookup(CredentialKind kind, const std::string& accountId, LookupCallback callback) {
    auto& cache = kind == CredentialKind::Password ? m_passwords : m_refreshTokens;
    if (const auto it = cache.find(accountId); it != cache.end()) {
      callback(security::SecretStoreStatus::Success, it->second);
      return;
    }

    m_secretStore.lookup(
        secretId(kind, accountId),
        [this, kind, accountId,
         callback = std::move(callback)](security::SecretStoreStatus status, security::SecureBuffer value) mutable {
          const bool statusChanged = updateRefreshTokenStatus(kind, accountId, status);
          if (status == security::SecretStoreStatus::Success) {
            auto secret = std::make_shared<security::SecureBuffer>(std::move(value));
            auto& target = kind == CredentialKind::Password ? m_passwords : m_refreshTokens;
            target.insert_or_assign(accountId, secret);
            setState(CredentialState::Ready, m_migrationPending, statusChanged);
            callback(status, std::move(secret));
            return;
          }
          setState(stateForStatus(status), m_migrationPending, statusChanged);
          callback(status, {});
        }
    );
  }

  void CalendarCredentialStore::storePassword(
      const std::string& accountId, security::SecureBuffer value, StatusCallback callback
  ) {
    store(CredentialKind::Password, accountId, std::move(value), std::move(callback));
  }

  void CalendarCredentialStore::storeRefreshToken(
      const std::string& accountId, security::SecureBuffer value, StatusCallback callback
  ) {
    store(CredentialKind::RefreshToken, accountId, std::move(value), std::move(callback));
  }

  void CalendarCredentialStore::store(
      CredentialKind kind, const std::string& accountId, security::SecureBuffer value, StatusCallback callback
  ) {
    auto cachedValue = std::make_shared<security::SecureBuffer>(value.bytes());
    m_secretStore.store(
        secretId(kind, accountId), std::move(value), label(kind),
        [this, kind, accountId, cachedValue = std::move(cachedValue),
         callback = std::move(callback)](security::SecretStoreStatus status) mutable {
          const bool statusChanged = updateRefreshTokenStatus(kind, accountId, status);
          if (status == security::SecretStoreStatus::Success) {
            auto& cache = kind == CredentialKind::Password ? m_passwords : m_refreshTokens;
            cache.insert_or_assign(accountId, std::move(cachedValue));
            setState(CredentialState::Ready, m_migrationPending, statusChanged);
          } else {
            setState(stateForStatus(status), m_migrationPending, statusChanged);
          }
          if (callback) {
            callback(status);
          }
        }
    );
  }

  void CalendarCredentialStore::eraseRefreshToken(const std::string& accountId, StatusCallback callback) {
    m_refreshTokens.erase(accountId);
    const bool statusChanged =
        updateRefreshTokenStatus(CredentialKind::RefreshToken, accountId, security::SecretStoreStatus::NotFound);
    setState(CredentialState::Ready, m_migrationPending, statusChanged);
    erase(CredentialKind::RefreshToken, accountId, std::move(callback));
  }

  void CalendarCredentialStore::erasePassword(const std::string& accountId, StatusCallback callback) {
    erase(CredentialKind::Password, accountId, std::move(callback));
  }

  void CalendarCredentialStore::erase(CredentialKind kind, const std::string& accountId, StatusCallback callback) {
    m_secretStore.erase(
        secretId(kind, accountId),
        [this, kind, accountId, callback = std::move(callback)](security::SecretStoreStatus status) mutable {
          const bool erased =
              status == security::SecretStoreStatus::Success || status == security::SecretStoreStatus::NotFound;
          const auto effectiveStatus = erased ? security::SecretStoreStatus::NotFound : status;
          const bool statusChanged = updateRefreshTokenStatus(kind, accountId, effectiveStatus);
          if (erased) {
            auto& cache = kind == CredentialKind::Password ? m_passwords : m_refreshTokens;
            cache.erase(accountId);
            setState(CredentialState::Ready, m_migrationPending, statusChanged);
          } else {
            setState(stateForStatus(status), m_migrationPending, statusChanged);
          }
          if (callback) {
            callback(status);
          }
        }
    );
  }

  void CalendarCredentialStore::eraseAccount(const std::string& accountId, StatusCallback callback) {
    erase(
        CredentialKind::Password, accountId,
        [this, accountId, callback = std::move(callback)](security::SecretStoreStatus passwordStatus) mutable {
          if (passwordStatus != security::SecretStoreStatus::Success
              && passwordStatus != security::SecretStoreStatus::NotFound) {
            if (callback) {
              callback(passwordStatus);
            }
            return;
          }
          erase(CredentialKind::RefreshToken, accountId, std::move(callback));
        }
    );
  }

  void CalendarCredentialStore::retainAccounts(const std::unordered_set<std::string>& accountIds) {
    const auto removeUnknown = [&accountIds](auto& values) {
      std::erase_if(values, [&accountIds](const auto& item) { return !accountIds.contains(item.first); });
    };
    removeUnknown(m_passwords);
    removeUnknown(m_refreshTokens);
    removeUnknown(m_refreshTokenStatuses);
  }

} // namespace calendar
