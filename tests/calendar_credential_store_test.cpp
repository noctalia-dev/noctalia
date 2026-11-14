#include "calendar/calendar_credential_store.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using security::SecretStoreBackendResult;
  using security::SecretStoreErrorCategory;
  using security::SecretStoreStatus;

  std::string keyFor(const security::SecretStoreAttributes& attributes) {
    return attributes.scope + '/' + attributes.owner + '/' + attributes.name;
  }

  security::SecureBuffer secret(std::string_view value) {
    return security::SecureBuffer(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
  }

  class TempCredentialFile {
  public:
    explicit TempCredentialFile(std::span<const std::uint8_t> contents) {
      std::array<char, 64> pattern{};
      const std::string templatePath = "/tmp/noctalia-calendar-credential-XXXXXX";
      std::ranges::copy(templatePath, pattern.begin());
      const int fd = ::mkstemp(pattern.data());
      if (fd < 0) {
        return;
      }
      m_path = pattern.data();
      std::size_t offset = 0;
      while (offset < contents.size()) {
        const ssize_t count = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (count <= 0) {
          break;
        }
        offset += static_cast<std::size_t>(count);
      }
      ::close(fd);
      if (offset != contents.size()) {
        std::filesystem::remove(m_path);
        m_path.clear();
      }
    }

    ~TempCredentialFile() {
      if (!m_path.empty()) {
        std::filesystem::remove(m_path);
      }
    }

    TempCredentialFile(const TempCredentialFile&) = delete;
    TempCredentialFile& operator=(const TempCredentialFile&) = delete;

    const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  std::string stringValue(const calendar::CalendarCredentialStore::Secret& value) {
    if (!value) {
      return {};
    }
    const auto bytes = value->bytes();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }

  class FakeBackend final : public security::SecretStoreBackend {
  public:
    SecretStoreBackendResult probe(security::SecretStoreCancellation&) override { return success(); }

    SecretStoreBackendResult
    lookup(const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation&) override {
      std::scoped_lock lock(m_mutex);
      ++m_calls;
      if (m_lookupStatus.has_value()) {
        return {
            .status = *m_lookupStatus,
            .errorCategory = *m_lookupStatus == SecretStoreStatus::DeniedOrLocked ? SecretStoreErrorCategory::Locked
                                                                                  : SecretStoreErrorCategory::None,
        };
      }
      const auto it = m_values.find(keyFor(attributes));
      if (it == m_values.end()) {
        return {.status = SecretStoreStatus::NotFound, .errorCategory = SecretStoreErrorCategory::None};
      }
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
          .value = security::SecureBuffer(it->second),
      };
    }

    SecretStoreBackendResult store(
        const security::SecretStoreAttributes& attributes, std::span<const std::uint8_t> value, const std::string&,
        security::SecretStoreCancellation&
    ) override {
      std::scoped_lock lock(m_mutex);
      ++m_calls;
      ++m_storeCalls;
      if (m_failStoreCall.has_value() && m_storeCalls == *m_failStoreCall) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .errorCategory = SecretStoreErrorCategory::ProviderUnavailable,
        };
      }
      m_values.insert_or_assign(keyFor(attributes), std::vector<std::uint8_t>(value.begin(), value.end()));
      return success();
    }

    SecretStoreBackendResult
    erase(const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation&) override {
      std::scoped_lock lock(m_mutex);
      ++m_calls;
      if (m_values.erase(keyFor(attributes)) == 0) {
        return {.status = SecretStoreStatus::NotFound, .errorCategory = SecretStoreErrorCategory::None};
      }
      return success();
    }

    void failStoreCall(std::optional<std::size_t> call) {
      std::scoped_lock lock(m_mutex);
      m_failStoreCall = call;
    }

    void setLookupStatus(std::optional<SecretStoreStatus> status) {
      std::scoped_lock lock(m_mutex);
      m_lookupStatus = status;
    }

    void seed(std::string_view key, std::string_view value) {
      std::scoped_lock lock(m_mutex);
      m_values.insert_or_assign(
          std::string(key),
          std::vector<std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(value.data()),
              reinterpret_cast<const std::uint8_t*>(value.data() + value.size())
          )
      );
    }

    std::optional<std::string> value(std::string_view key) const {
      std::scoped_lock lock(m_mutex);
      const auto it = m_values.find(std::string(key));
      if (it == m_values.end()) {
        return std::nullopt;
      }
      return std::string(reinterpret_cast<const char*>(it->second.data()), it->second.size());
    }

    std::size_t calls() const {
      std::scoped_lock lock(m_mutex);
      return m_calls;
    }

  private:
    static SecretStoreBackendResult success() {
      return {.status = SecretStoreStatus::Success, .errorCategory = SecretStoreErrorCategory::None};
    }

    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<std::uint8_t>> m_values;
    std::optional<SecretStoreStatus> m_lookupStatus;
    std::optional<std::size_t> m_failStoreCall;
    std::size_t m_calls = 0;
    std::size_t m_storeCalls = 0;
  };

  void dispatchOne(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    TEST_CHECK(start == 0 && fds.size() == 1);
    // Completion timed out.
    TEST_CHECK(::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000) == 1);
    store.dispatch(fds, start);
  }

  calendar::CredentialMigration migration(bool& finalized) {
    calendar::CredentialMigration input;
    input.credentials.push_back({
        .accountId = "personal",
        .kind = calendar::CredentialKind::Password,
        .value = secret("app-password"),
    });
    input.credentials.push_back({
        .accountId = "work",
        .kind = calendar::CredentialKind::RefreshToken,
        .value = secret("refresh-token"),
    });
    input.finalize = [&finalized] {
      finalized = true;
      return true;
    };
    return input;
  }

  void migrationSuccessAndCache() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    bool finalized = false;
    std::optional<SecretStoreStatus> status;
    credentials.initialize(migration(finalized), [&status](SecretStoreStatus value) { status = value; });

    dispatchOne(secretStore);
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(finalized);
    TEST_CHECK(credentials.state() == calendar::CredentialState::Ready);
    TEST_CHECK(!credentials.migrationPending());
    TEST_CHECK(fake->value("calendar/personal/password") == "app-password");
    TEST_CHECK(fake->value("calendar/work/refresh-token") == "refresh-token");

    const std::size_t callsBeforeLookup = fake->calls();
    std::string loaded;
    credentials.lookupPassword(
        "personal", [&loaded](SecretStoreStatus lookupStatus, const calendar::CalendarCredentialStore::Secret& value) {
          if (lookupStatus == SecretStoreStatus::Success) {
            loaded = stringValue(value);
          }
        }
    );
    TEST_CHECK(loaded == "app-password");
    TEST_CHECK(fake->calls() == callsBeforeLookup);
  }

  void interruptedMigrationRetriesIdempotently() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    fake->failStoreCall(2);
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    bool finalized = false;
    std::optional<SecretStoreStatus> status;
    credentials.initialize(migration(finalized), [&status](SecretStoreStatus value) { status = value; });

    dispatchOne(secretStore);
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::Unavailable);
    TEST_CHECK(!finalized);
    TEST_CHECK(credentials.migrationPending());
    TEST_CHECK(credentials.state() == calendar::CredentialState::Unavailable);

    fake->failStoreCall(std::nullopt);
    status.reset();
    bool retryFinalized = false;
    credentials.retryMigration(migration(retryFinalized), [&status](SecretStoreStatus value) { status = value; });
    dispatchOne(secretStore); // availability probe
    dispatchOne(secretStore); // idempotent password replacement
    dispatchOne(secretStore); // refresh token
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(retryFinalized);
    TEST_CHECK(!credentials.migrationPending());
  }

  void accountEraseTreatsMissingAsSuccess() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    std::optional<SecretStoreStatus> status;
    credentials.eraseAccount("missing", [&status](SecretStoreStatus value) { status = value; });

    dispatchOne(secretStore);
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::NotFound);
    TEST_CHECK(fake->calls() == 2);
  }

  void missingRefreshTokenSignalsReconnectState() {
    auto backend = std::make_unique<FakeBackend>();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    calendar::CredentialMigration complete;
    complete.complete = true;
    credentials.initialize(std::move(complete));

    std::size_t changes = 0;
    credentials.setChangeCallback([&changes] { ++changes; });
    std::optional<SecretStoreStatus> status;
    credentials.lookupRefreshToken(
        "personal",
        [&status](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret&) { status = value; }
    );

    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::NotFound);
    TEST_CHECK(credentials.refreshTokenMissing("personal"));
    TEST_CHECK(changes == 1);

    status.reset();
    credentials.storeRefreshToken("personal", secret("replacement-token"), [&status](SecretStoreStatus value) {
      status = value;
    });
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(!credentials.refreshTokenMissing("personal"));
    TEST_CHECK(changes == 2);
  }

  void missingRefreshTokenIsRetried() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    calendar::CredentialMigration complete;
    complete.complete = true;
    credentials.initialize(std::move(complete));

    std::optional<SecretStoreStatus> status;
    credentials.lookupRefreshToken(
        "personal",
        [&status](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret&) { status = value; }
    );
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::NotFound);
    TEST_CHECK(credentials.refreshTokenMissing("personal"));

    fake->seed("calendar/personal/refresh-token", "late-token");
    status.reset();
    std::string loaded;
    credentials.lookupRefreshToken(
        "personal",
        [&status, &loaded](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret& secretValue) {
          status = value;
          loaded = stringValue(secretValue);
        }
    );
    TEST_CHECK(!status.has_value());
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(loaded == "late-token");
    TEST_CHECK(!credentials.refreshTokenMissing("personal"));

    const std::size_t callsBeforeCachedLookup = fake->calls();
    credentials.lookupRefreshToken(
        "personal",
        [&status, &loaded](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret& secretValue) {
          status = value;
          loaded = stringValue(secretValue);
        }
    );
    TEST_CHECK(status == SecretStoreStatus::Success && loaded == "late-token");
    TEST_CHECK(fake->calls() == callsBeforeCachedLookup);
  }

  void lockedRefreshTokenIsReportedAndRecovers() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    fake->setLookupStatus(SecretStoreStatus::DeniedOrLocked);
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    calendar::CredentialMigration complete;
    complete.complete = true;
    credentials.initialize(std::move(complete));

    std::size_t changes = 0;
    credentials.setChangeCallback([&changes] { ++changes; });
    std::optional<SecretStoreStatus> status;
    credentials.lookupRefreshToken(
        "personal",
        [&status](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret&) { status = value; }
    );

    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::DeniedOrLocked);
    TEST_CHECK(credentials.refreshTokenLocked("personal"));
    TEST_CHECK(credentials.anyRefreshTokenLocked());
    TEST_CHECK(!credentials.refreshTokenMissing("personal"));
    TEST_CHECK(changes == 1);

    fake->setLookupStatus(std::nullopt);
    fake->seed("calendar/personal/refresh-token", "unlocked-token");
    status.reset();
    credentials.lookupRefreshToken(
        "personal",
        [&status](SecretStoreStatus value, const calendar::CalendarCredentialStore::Secret&) { status = value; }
    );
    dispatchOne(secretStore);
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(!credentials.refreshTokenLocked("personal"));
    TEST_CHECK(!credentials.anyRefreshTokenLocked());
    TEST_CHECK(changes == 2);
  }

  void completedMigrationDoesNotReadLegacyInput() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    bool finalized = false;
    calendar::CredentialMigration input;
    input.complete = true;
    input.credentials.push_back({
        .accountId = "legacy",
        .kind = calendar::CredentialKind::Password,
        .value = secret("must-not-be-stored"),
    });
    input.finalize = [&finalized] {
      finalized = true;
      return true;
    };

    std::optional<SecretStoreStatus> status;
    credentials.initialize(std::move(input), [&status](SecretStoreStatus value) { status = value; });
    TEST_CHECK(status == SecretStoreStatus::Success);
    TEST_CHECK(!finalized);
    TEST_CHECK(fake->calls() == 0);
    TEST_CHECK(!fake->value("calendar/legacy/password").has_value());
  }

  void credentialFileReadIsStrict() {
    const std::string password = "agenix-password\r\n";
    const std::array<std::uint8_t, 4> nulValue = {'a', 0, 'b', '\n'};
    const std::array<std::uint8_t, 1> newline = {'\n'};
    calendar::CredentialFileResult loaded;
    calendar::CredentialFileStatus embeddedNulStatus{};
    calendar::CredentialFileStatus emptyAfterTrimStatus{};
    {
      const TempCredentialFile valid(
          std::span(reinterpret_cast<const std::uint8_t*>(password.data()), password.size())
      );
      loaded = calendar::readCredentialFile(valid.path());
      const TempCredentialFile embeddedNul(nulValue);
      embeddedNulStatus = calendar::readCredentialFile(embeddedNul.path()).status;
      const TempCredentialFile emptyAfterTrim(newline);
      emptyAfterTrimStatus = calendar::readCredentialFile(emptyAfterTrim.path()).status;
    }

    TEST_CHECK(loaded.status == calendar::CredentialFileStatus::Success);
    // The credential file line ending is stripped.
    TEST_CHECK(
        std::string(reinterpret_cast<const char*>(loaded.value.bytes().data()), loaded.value.size())
        == "agenix-password"
    );
    TEST_CHECK(embeddedNulStatus == calendar::CredentialFileStatus::Invalid);
    TEST_CHECK(emptyAfterTrimStatus == calendar::CredentialFileStatus::Invalid);
    TEST_CHECK(
        calendar::readCredentialFile("/tmp/noctalia-calendar-credential-definitely-missing").status
        == calendar::CredentialFileStatus::NotFound
    );
  }
} // namespace

int main() {
  TEST_CHECK(security::initializeSecurityPrimitives());

  migrationSuccessAndCache();
  interruptedMigrationRetriesIdempotently();
  accountEraseTreatsMissingAsSuccess();
  missingRefreshTokenSignalsReconnectState();
  missingRefreshTokenIsRetried();
  lockedRefreshTokenIsReportedAndRecovers();
  completedMigrationDoesNotReadLegacyInput();
  credentialFileReadIsStrict();
  return 0;
}
