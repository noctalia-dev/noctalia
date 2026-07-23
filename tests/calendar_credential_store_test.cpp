#include "calendar/calendar_credential_store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <print>
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

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "calendar_credential_store_test: {}", message);
    }
    return condition;
  }

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

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

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

    [[nodiscard]] std::optional<std::string> value(std::string_view key) const {
      std::scoped_lock lock(m_mutex);
      const auto it = m_values.find(std::string(key));
      if (it == m_values.end()) {
        return std::nullopt;
      }
      return std::string(reinterpret_cast<const char*>(it->second.data()), it->second.size());
    }

    [[nodiscard]] std::size_t calls() const {
      std::scoped_lock lock(m_mutex);
      return m_calls;
    }

  private:
    static SecretStoreBackendResult success() {
      return {.status = SecretStoreStatus::Success, .errorCategory = SecretStoreErrorCategory::None};
    }

    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<std::uint8_t>> m_values;
    std::optional<std::size_t> m_failStoreCall;
    std::size_t m_calls = 0;
    std::size_t m_storeCalls = 0;
  };

  bool dispatchOne(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    if (!expect(start == 0 && fds.size() == 1, "SecretStore poll fd missing")) {
      return false;
    }
    if (!expect(::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000) == 1, "completion timed out")) {
      return false;
    }
    store.dispatch(fds, start);
    return true;
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
    input.finalize = [&finalized]() {
      finalized = true;
      return true;
    };
    return input;
  }

  bool migrationSuccessAndCache() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    bool finalized = false;
    std::optional<SecretStoreStatus> status;
    credentials.initialize(migration(finalized), [&status](SecretStoreStatus value) { status = value; });

    bool ok = dispatchOne(secretStore);
    ok = dispatchOne(secretStore) && ok;
    ok = expect(status == SecretStoreStatus::Success, "migration did not complete") && ok;
    ok = expect(finalized, "migration state was not finalized") && ok;
    ok = expect(credentials.state() == calendar::CredentialState::Ready, "credential state is not ready") && ok;
    ok = expect(!credentials.migrationPending(), "completed migration remained pending") && ok;
    ok = expect(fake->value("calendar/personal/password") == "app-password", "password was not migrated") && ok;
    ok = expect(fake->value("calendar/work/refresh-token") == "refresh-token", "refresh token was not migrated") && ok;

    const std::size_t callsBeforeLookup = fake->calls();
    std::string loaded;
    credentials.lookupPassword(
        "personal", [&loaded](SecretStoreStatus lookupStatus, calendar::CalendarCredentialStore::Secret value) {
          if (lookupStatus == SecretStoreStatus::Success) {
            loaded = stringValue(value);
          }
        }
    );
    ok = expect(loaded == "app-password", "cached password lookup failed") && ok;
    ok = expect(fake->calls() == callsBeforeLookup, "cached password hit the backend") && ok;
    return ok;
  }

  bool interruptedMigrationRetriesIdempotently() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    fake->failStoreCall(2);
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    bool finalized = false;
    std::optional<SecretStoreStatus> status;
    credentials.initialize(migration(finalized), [&status](SecretStoreStatus value) { status = value; });

    bool ok = dispatchOne(secretStore);
    ok = dispatchOne(secretStore) && ok;
    ok = expect(status == SecretStoreStatus::Unavailable, "migration failure status was lost") && ok;
    ok = expect(!finalized, "failed migration finalized state") && ok;
    ok = expect(credentials.migrationPending(), "failed migration did not remain pending") && ok;
    ok = expect(credentials.state() == calendar::CredentialState::Unavailable, "unavailable state was not exposed")
        && ok;

    fake->failStoreCall(std::nullopt);
    status.reset();
    bool retryFinalized = false;
    credentials.retryMigration(migration(retryFinalized), [&status](SecretStoreStatus value) { status = value; });
    ok = dispatchOne(secretStore) && ok; // availability probe
    ok = dispatchOne(secretStore) && ok; // idempotent password replacement
    ok = dispatchOne(secretStore) && ok; // refresh token
    ok = expect(status == SecretStoreStatus::Success, "migration retry did not complete") && ok;
    ok = expect(retryFinalized, "migration retry did not finalize state") && ok;
    ok = expect(!credentials.migrationPending(), "migration retry remained pending") && ok;
    return ok;
  }

  bool accountEraseTreatsMissingAsSuccess() {
    auto backend = std::make_unique<FakeBackend>();
    auto* fake = backend.get();
    security::SecretStore secretStore(std::move(backend));
    calendar::CalendarCredentialStore credentials(secretStore);
    std::optional<SecretStoreStatus> status;
    credentials.eraseAccount("missing", [&status](SecretStoreStatus value) { status = value; });

    bool ok = dispatchOne(secretStore);
    ok = dispatchOne(secretStore) && ok;
    ok = expect(status == SecretStoreStatus::NotFound, "missing account erase returned an error") && ok;
    ok = expect(fake->calls() == 2, "account erase did not check both credential kinds") && ok;
    return ok;
  }

  bool completedMigrationDoesNotReadLegacyInput() {
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
    input.finalize = [&finalized]() {
      finalized = true;
      return true;
    };

    std::optional<SecretStoreStatus> status;
    credentials.initialize(std::move(input), [&status](SecretStoreStatus value) { status = value; });
    bool ok = true;
    ok = expect(status == SecretStoreStatus::Success, "completed migration did not initialize") && ok;
    ok = expect(!finalized, "completed migration ran its legacy finalizer") && ok;
    ok = expect(fake->calls() == 0, "completed migration touched the secret backend") && ok;
    ok = expect(!fake->value("calendar/legacy/password").has_value(), "completed migration stored legacy input") && ok;
    return ok;
  }

  bool credentialFileReadIsStrict() {
    const std::string password = "agenix-password\r\n";
    const TempCredentialFile valid(std::span(reinterpret_cast<const std::uint8_t*>(password.data()), password.size()));
    calendar::CredentialFileResult loaded = calendar::readCredentialFile(valid.path());
    bool ok = expect(loaded.status == calendar::CredentialFileStatus::Success, "valid credential file was rejected");
    ok = expect(
             std::string(reinterpret_cast<const char*>(loaded.value.bytes().data()), loaded.value.size())
                 == "agenix-password",
             "credential file line ending was not stripped"
         )
        && ok;

    const std::array<std::uint8_t, 4> nulValue = {'a', 0, 'b', '\n'};
    const TempCredentialFile embeddedNul(nulValue);
    ok = expect(
             calendar::readCredentialFile(embeddedNul.path()).status == calendar::CredentialFileStatus::Invalid,
             "credential file with embedded NUL was accepted"
         )
        && ok;

    const std::array<std::uint8_t, 1> newline = {'\n'};
    const TempCredentialFile emptyAfterTrim(newline);
    ok = expect(
             calendar::readCredentialFile(emptyAfterTrim.path()).status == calendar::CredentialFileStatus::Invalid,
             "empty credential file was accepted"
         )
        && ok;
    ok = expect(
             calendar::readCredentialFile("/tmp/noctalia-calendar-credential-definitely-missing").status
                 == calendar::CredentialFileStatus::NotFound,
             "missing credential file status was not preserved"
         )
        && ok;
    return ok;
  }
} // namespace

int main() {
  if (!security::initializeSecurityPrimitives()) {
    std::println(stderr, "calendar_credential_store_test: sodium initialization failed");
    return 1;
  }

  bool ok = true;
  ok = migrationSuccessAndCache() && ok;
  ok = interruptedMigrationRetriesIdempotently() && ok;
  ok = accountEraseTreatsMissingAsSuccess() && ok;
  ok = completedMigrationDoesNotReadLegacyInput() && ok;
  ok = credentialFileReadIsStrict() && ok;
  return ok ? 0 : 1;
}
