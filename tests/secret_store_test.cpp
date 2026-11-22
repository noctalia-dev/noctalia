#include "security/secret_store.h"
#include "tests/test_check.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using security::SecretStoreBackendResult;
  using security::SecretStoreErrorCategory;
  using security::SecretStoreStatus;

  std::string keyFor(const security::SecretStoreAttributes& attributes) {
    return attributes.application
        + '\x1f'
        + attributes.scope
        + '\x1f'
        + attributes.owner
        + '\x1f'
        + attributes.name
        + '\x1f'
        + attributes.version;
  }

  class FakeSecretStoreBackend final : public security::SecretStoreBackend {
  public:
    SecretStoreBackendResult probe(security::SecretStoreCancellation& cancellation) override {
      if (auto forced = prepare(cancellation)) {
        return std::move(*forced);
      }
      return success();
    }

    SecretStoreBackendResult lookup(
        const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation& cancellation
    ) override {
      recordAttributes(attributes);
      if (auto forced = prepare(cancellation)) {
        return std::move(*forced);
      }
      std::scoped_lock lock(m_mutex);
      const auto it = m_values.find(keyFor(attributes));
      if (it == m_values.end()) {
        return {
            .status = SecretStoreStatus::NotFound,
            .errorCategory = SecretStoreErrorCategory::None,
        };
      }
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
          .value = security::SecureBuffer(it->second),
      };
    }

    SecretStoreBackendResult store(
        const security::SecretStoreAttributes& attributes, std::span<const std::uint8_t> value,
        const std::string& label, security::SecretStoreCancellation& cancellation
    ) override {
      recordAttributes(attributes);
      {
        std::scoped_lock lock(m_mutex);
        m_lastLabel = label;
      }
      if (auto forced = prepare(cancellation)) {
        return std::move(*forced);
      }
      std::scoped_lock lock(m_mutex);
      m_values.insert_or_assign(keyFor(attributes), std::vector<std::uint8_t>(value.begin(), value.end()));
      return success();
    }

    SecretStoreBackendResult
    erase(const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation& cancellation) override {
      recordAttributes(attributes);
      if (auto forced = prepare(cancellation)) {
        return std::move(*forced);
      }
      std::scoped_lock lock(m_mutex);
      if (m_values.erase(keyFor(attributes)) == 0) {
        return {
            .status = SecretStoreStatus::NotFound,
            .errorCategory = SecretStoreErrorCategory::None,
        };
      }
      return success();
    }

    void force(SecretStoreStatus status, SecretStoreErrorCategory category) {
      std::scoped_lock lock(m_mutex);
      m_forced = SecretStoreBackendResult{.status = status, .errorCategory = category};
    }

    void clearForced() {
      std::scoped_lock lock(m_mutex);
      m_forced.reset();
    }

    void setBlocked(bool blocked) {
      {
        std::scoped_lock lock(m_mutex);
        m_blocked = blocked;
      }
      m_cv.notify_all();
    }

    bool waitForCalls(std::size_t expected) {
      std::unique_lock lock(m_mutex);
      return m_cv.wait_for(lock, std::chrono::seconds(2), [this, expected] { return m_calls >= expected; });
    }

    std::size_t calls() const {
      std::scoped_lock lock(m_mutex);
      return m_calls;
    }

    std::optional<security::SecretStoreAttributes> lastAttributes() const {
      std::scoped_lock lock(m_mutex);
      return m_lastAttributes;
    }

    std::thread::id workerThread() const {
      std::scoped_lock lock(m_mutex);
      return m_workerThread;
    }

  private:
    static SecretStoreBackendResult success() {
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
      };
    }

    void recordAttributes(const security::SecretStoreAttributes& attributes) {
      std::scoped_lock lock(m_mutex);
      m_lastAttributes = attributes;
    }

    std::optional<SecretStoreBackendResult> prepare(security::SecretStoreCancellation& cancellation) {
      cancellation.setCancelCallback([this] { m_cv.notify_all(); });
      std::unique_lock lock(m_mutex);
      ++m_calls;
      m_workerThread = std::this_thread::get_id();
      m_cv.notify_all();
      m_cv.wait(lock, [this, &cancellation] { return !m_blocked || cancellation.cancelled(); });
      std::optional<SecretStoreBackendResult> result;
      if (cancellation.cancelled()) {
        result = SecretStoreBackendResult{
            .status = SecretStoreStatus::Cancelled,
            .errorCategory = SecretStoreErrorCategory::Cancelled,
        };
      } else if (m_forced.has_value()) {
        result = SecretStoreBackendResult{
            .status = m_forced->status,
            .errorCategory = m_forced->errorCategory,
        };
      }
      lock.unlock();
      cancellation.clearCancelCallback();
      return result;
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, std::vector<std::uint8_t>> m_values;
    std::optional<SecretStoreBackendResult> m_forced;
    std::optional<security::SecretStoreAttributes> m_lastAttributes;
    std::string m_lastLabel;
    std::thread::id m_workerThread;
    std::size_t m_calls = 0;
    bool m_blocked = false;
  };

  void dispatchCompletion(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    TEST_CHECK(start == 0 && fds.size() == 1);
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000);
    TEST_CHECK(ready == 1);
    store.dispatch(fds, start);
  }

  void exactIdentityAndValidation() {
    const security::SecretId id{.scope = "calendar", .owner = "personal", .name = "refresh-token"};
    const auto attributes = security::secretStoreAttributes(id);
    TEST_CHECK(security::SecretStoreSchemaName == "dev.noctalia.Secret");
    TEST_CHECK(attributes.application == "noctalia");
    TEST_CHECK(attributes.scope == "calendar");
    TEST_CHECK(attributes.owner == "personal");
    TEST_CHECK(attributes.name == "refresh-token");
    TEST_CHECK(attributes.version == "1");
    TEST_CHECK(security::isValidSecretId(id));
    TEST_CHECK(security::isValidSecretId({.scope = "storage", .owner = "encrypted-state", .name = "master-key"}));
    TEST_CHECK(!security::isValidSecretId({.scope = "cache", .owner = "clipboard-cache-v1", .name = "data-key"}));
    TEST_CHECK(!security::isValidSecretId({.scope = "calendar", .owner = "personal", .name = "access-token"}));
    TEST_CHECK(!security::isValidSecretId({.scope = "cache", .owner = "", .name = "data-key"}));

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    std::optional<SecretStoreStatus> status;
    store.lookup(
        {.scope = "calendar", .owner = "personal", .name = "access-token"},
        [&status](SecretStoreStatus value, security::SecureBuffer) { status = value; }
    );
    dispatchCompletion(store);
    TEST_CHECK(status == SecretStoreStatus::BackendError);
    TEST_CHECK(fake->calls() == 0);
  }

  void binaryReplacementAndDeletion() {
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    const security::SecretId id{.scope = "storage", .owner = "encrypted-state", .name = "master-key"};
    const std::array<std::uint8_t, 5> first = {1, 0, 2, 0, 3};
    const std::array<std::uint8_t, 4> replacement = {9, 0, 8, 7};

    std::optional<SecretStoreStatus> status;
    store.store(id, security::SecureBuffer(first), "Noctalia clipboard key", [&status](SecretStoreStatus value) {
      status = value;
    });
    dispatchCompletion(store);
    TEST_CHECK(status == SecretStoreStatus::Success);

    status.reset();
    store.store(id, security::SecureBuffer(replacement), "Noctalia clipboard key", [&status](SecretStoreStatus value) {
      status = value;
    });
    dispatchCompletion(store);
    TEST_CHECK(status == SecretStoreStatus::Success);

    std::optional<SecretStoreStatus> lookupStatus;
    std::vector<std::uint8_t> loaded;
    store.lookup(id, [&lookupStatus, &loaded](SecretStoreStatus value, security::SecureBuffer secret) {
      lookupStatus = value;
      loaded.assign(secret.bytes().begin(), secret.bytes().end());
    });
    dispatchCompletion(store);
    TEST_CHECK(lookupStatus == SecretStoreStatus::Success);
    TEST_CHECK(loaded == std::vector<std::uint8_t>(replacement.begin(), replacement.end()));

    status.reset();
    store.erase(id, [&status](SecretStoreStatus value) { status = value; });
    dispatchCompletion(store);
    TEST_CHECK(status == SecretStoreStatus::Success);
    status.reset();
    store.erase(id, [&status](SecretStoreStatus value) { status = value; });
    dispatchCompletion(store);
    TEST_CHECK(status == SecretStoreStatus::NotFound);

    const auto recorded = fake->lastAttributes();
    TEST_CHECK(recorded.has_value());
    TEST_CHECK(recorded == security::secretStoreAttributes(id));
  }

  void statusAndThreadDispatch() {
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    const auto mainThread = std::this_thread::get_id();
    const security::SecretId id{.scope = "calendar", .owner = "work", .name = "password"};
    const std::array cases = {
        std::pair{SecretStoreStatus::NotFound, SecretStoreErrorCategory::None},
        std::pair{SecretStoreStatus::Unavailable, SecretStoreErrorCategory::ProviderUnavailable},
        std::pair{SecretStoreStatus::Cancelled, SecretStoreErrorCategory::Cancelled},
        std::pair{SecretStoreStatus::DeniedOrLocked, SecretStoreErrorCategory::Locked},
        std::pair{SecretStoreStatus::BackendError, SecretStoreErrorCategory::Protocol},
    };
    for (const auto& [expected, category] : cases) {
      fake->force(expected, category);
      std::optional<SecretStoreStatus> received;
      std::thread::id callbackThread;
      store.lookup(id, [&received, &callbackThread](SecretStoreStatus status, security::SecureBuffer) {
        received = status;
        callbackThread = std::this_thread::get_id();
      });
      dispatchCompletion(store);
      TEST_CHECK(received == expected);
      TEST_CHECK(callbackThread == mainThread);
      TEST_CHECK(fake->workerThread() != mainThread);
    }

    fake->clearForced();
    std::optional<SecretStoreStatus> probeStatus;
    store.retryAvailabilityCheck([&probeStatus](SecretStoreStatus status) { probeStatus = status; });
    dispatchCompletion(store);
    TEST_CHECK(probeStatus == SecretStoreStatus::Success);
    TEST_CHECK(store.availabilityStatus() == SecretStoreStatus::Success);
  }

  void cancellationAndDestruction() {
    {
      auto backend = std::make_unique<FakeSecretStoreBackend>();
      auto* fake = backend.get();
      fake->setBlocked(true);
      security::SecretStore store(std::move(backend));
      bool callbackCalled = false;
      auto operation = store.lookup(
          {.scope = "calendar", .owner = "personal", .name = "password"},
          [&callbackCalled](SecretStoreStatus, security::SecureBuffer) { callbackCalled = true; }
      );
      TEST_CHECK(fake->waitForCalls(1));
      operation.cancel();
      dispatchCompletion(store);
      TEST_CHECK(operation.cancelled());
      TEST_CHECK(!callbackCalled);
    }

    bool callbackCalled = false;
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    fake->setBlocked(true);
    {
      security::SecretStore store(std::move(backend));
      store.lookup(
          {.scope = "calendar", .owner = "personal", .name = "password"},
          [&callbackCalled](SecretStoreStatus, security::SecureBuffer) { callbackCalled = true; }
      );
      TEST_CHECK(fake->waitForCalls(1));
    }
    TEST_CHECK(!callbackCalled);
  }

  void failureLogsExcludeSecretMaterial() {
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    fake->force(SecretStoreStatus::BackendError, SecretStoreErrorCategory::Protocol);
    security::SecretStore store(std::move(backend));
    const std::string secretMarker = "secret-value-marker-4815162342";
    const std::string labelMarker = "secret-label-marker-108";
    const auto markerBytes = std::span(reinterpret_cast<const std::uint8_t*>(secretMarker.data()), secretMarker.size());
    store.store(
        {.scope = "calendar", .owner = "logging-test", .name = "password"}, security::SecureBuffer(markerBytes),
        labelMarker, [](SecretStoreStatus) {}
    );

    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    TEST_CHECK(::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000) == 1);

    std::array<int, 2> pipeFds{-1, -1};
    TEST_CHECK(::pipe(pipeFds.data()) == 0);
    std::fflush(stderr);
    const int savedStderr = ::dup(STDERR_FILENO);
    const bool redirected = savedStderr >= 0 && ::dup2(pipeFds[1], STDERR_FILENO) >= 0;
    if (!redirected) {
      ::close(pipeFds[0]);
      ::close(pipeFds[1]);
      if (savedStderr >= 0) {
        ::close(savedStderr);
      }
    }
    TEST_CHECK(redirected);

    store.dispatch(fds, start);
    std::fflush(stderr);
    (void)::dup2(savedStderr, STDERR_FILENO);
    ::close(savedStderr);
    ::close(pipeFds[1]);

    std::string captured;
    std::array<char, 512> buffer{};
    ssize_t count = 0;
    while ((count = ::read(pipeFds[0], buffer.data(), buffer.size())) > 0) {
      captured.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipeFds[0]);
    TEST_CHECK(captured.contains("backend-error"));
    TEST_CHECK(!captured.contains(secretMarker));
    TEST_CHECK(!captured.contains(labelMarker));
  }
} // namespace

int main() {
  exactIdentityAndValidation();
  binaryReplacementAndDeletion();
  statusAndThreadDispatch();
  cancellationAndDestruction();
  failureLogsExcludeSecretMaterial();
  return 0;
}
