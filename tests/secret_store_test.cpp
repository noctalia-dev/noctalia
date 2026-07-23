#include "security/secret_store.h"

#include <poll.h>
#include <unistd.h>

#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using security::SecretStoreBackendResult;
  using security::SecretStoreErrorCategory;
  using security::SecretStoreStatus;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "secret_store_test: {}", message);
    }
    return condition;
  }

  std::string keyFor(const security::SecretStoreAttributes& attributes) {
    return attributes.application + '\x1f' + attributes.scope + '\x1f' + attributes.owner + '\x1f'
        + attributes.name + '\x1f' + attributes.version;
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

    SecretStoreBackendResult erase(
        const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation& cancellation
    ) override {
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
      return m_cv.wait_for(lock, std::chrono::seconds(2), [this, expected]() { return m_calls >= expected; });
    }

    [[nodiscard]] std::size_t calls() const {
      std::scoped_lock lock(m_mutex);
      return m_calls;
    }

    [[nodiscard]] std::optional<security::SecretStoreAttributes> lastAttributes() const {
      std::scoped_lock lock(m_mutex);
      return m_lastAttributes;
    }

    [[nodiscard]] std::thread::id workerThread() const {
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
      cancellation.setCancelCallback([this]() { m_cv.notify_all(); });
      std::unique_lock lock(m_mutex);
      ++m_calls;
      m_workerThread = std::this_thread::get_id();
      m_cv.notify_all();
      m_cv.wait(lock, [this, &cancellation]() { return !m_blocked || cancellation.cancelled(); });
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

  bool dispatchCompletion(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    if (!expect(start == 0 && fds.size() == 1, "SecretStore did not publish exactly one poll fd")) {
      return false;
    }
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000);
    if (!expect(ready == 1, "timed out waiting for SecretStore completion")) {
      return false;
    }
    store.dispatch(fds, start);
    return true;
  }

  bool exactIdentityAndValidation() {
    const security::SecretId id{.scope = "calendar", .owner = "personal", .name = "refresh-token"};
    const auto attributes = security::secretStoreAttributes(id);
    bool ok = true;
    ok = expect(security::SecretStoreSchemaName == "dev.noctalia.Secret", "schema name changed") && ok;
    ok = expect(attributes.application == "noctalia", "application attribute changed") && ok;
    ok = expect(attributes.scope == "calendar", "scope attribute changed") && ok;
    ok = expect(attributes.owner == "personal", "owner attribute changed") && ok;
    ok = expect(attributes.name == "refresh-token", "name attribute changed") && ok;
    ok = expect(attributes.version == "1", "version attribute changed") && ok;
    ok = expect(security::isValidSecretId(id), "valid calendar id was rejected") && ok;
    ok = expect(
             security::isValidSecretId({.scope = "cache", .owner = "clipboard-cache-v1", .name = "data-key"}),
             "valid cache id was rejected"
         )
        && ok;
    ok = expect(
             !security::isValidSecretId({.scope = "calendar", .owner = "personal", .name = "access-token"}),
             "non-durable access token id was accepted"
         )
        && ok;
    ok = expect(
             !security::isValidSecretId({.scope = "cache", .owner = "", .name = "data-key"}),
             "empty owner was accepted"
         )
        && ok;

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    std::optional<SecretStoreStatus> status;
    store.lookup(
        {.scope = "calendar", .owner = "personal", .name = "access-token"},
        [&status](SecretStoreStatus value, security::SecureBuffer) { status = value; }
    );
    ok = dispatchCompletion(store) && ok;
    ok = expect(status == SecretStoreStatus::BackendError, "invalid id did not fail loudly") && ok;
    ok = expect(fake->calls() == 0, "invalid id reached the backend") && ok;
    return ok;
  }

  bool binaryReplacementAndDeletion() {
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    const security::SecretId id{.scope = "cache", .owner = "clipboard-cache-v1", .name = "data-key"};
    const std::array<std::uint8_t, 5> first = {1, 0, 2, 0, 3};
    const std::array<std::uint8_t, 4> replacement = {9, 0, 8, 7};
    bool ok = true;

    std::optional<SecretStoreStatus> status;
    store.store(id, security::SecureBuffer(first), "Noctalia clipboard key", [&status](SecretStoreStatus value) {
      status = value;
    });
    ok = dispatchCompletion(store) && ok;
    ok = expect(status == SecretStoreStatus::Success, "binary store failed") && ok;

    status.reset();
    store.store(id, security::SecureBuffer(replacement), "Noctalia clipboard key", [&status](SecretStoreStatus value) {
      status = value;
    });
    ok = dispatchCompletion(store) && ok;
    ok = expect(status == SecretStoreStatus::Success, "replacement store failed") && ok;

    std::optional<SecretStoreStatus> lookupStatus;
    std::vector<std::uint8_t> loaded;
    store.lookup(id, [&lookupStatus, &loaded](SecretStoreStatus value, security::SecureBuffer secret) {
      lookupStatus = value;
      loaded.assign(secret.bytes().begin(), secret.bytes().end());
    });
    ok = dispatchCompletion(store) && ok;
    ok = expect(lookupStatus == SecretStoreStatus::Success, "binary lookup failed") && ok;
    ok = expect(
             loaded == std::vector<std::uint8_t>(replacement.begin(), replacement.end()),
             "binary value containing NUL did not round-trip"
         )
        && ok;

    status.reset();
    store.erase(id, [&status](SecretStoreStatus value) { status = value; });
    ok = dispatchCompletion(store) && ok;
    ok = expect(status == SecretStoreStatus::Success, "exact-match erase failed") && ok;
    status.reset();
    store.erase(id, [&status](SecretStoreStatus value) { status = value; });
    ok = dispatchCompletion(store) && ok;
    ok = expect(status == SecretStoreStatus::NotFound, "missing erase did not report not-found") && ok;

    const auto recorded = fake->lastAttributes();
    ok = expect(recorded.has_value(), "backend did not receive attributes") && ok;
    ok = expect(recorded == security::secretStoreAttributes(id), "backend attributes were not exact") && ok;
    return ok;
  }

  bool statusAndThreadDispatch() {
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

    bool ok = true;
    for (const auto& [expected, category] : cases) {
      fake->force(expected, category);
      std::optional<SecretStoreStatus> received;
      std::thread::id callbackThread;
      store.lookup(id, [&received, &callbackThread](SecretStoreStatus status, security::SecureBuffer) {
        received = status;
        callbackThread = std::this_thread::get_id();
      });
      ok = dispatchCompletion(store) && ok;
      ok = expect(received == expected, "backend status was not preserved") && ok;
      ok = expect(callbackThread == mainThread, "callback did not run on dispatching thread") && ok;
      ok = expect(fake->workerThread() != mainThread, "backend operation ran on main thread") && ok;
    }

    fake->clearForced();
    std::optional<SecretStoreStatus> probeStatus;
    store.retryAvailabilityCheck([&probeStatus](SecretStoreStatus status) { probeStatus = status; });
    ok = dispatchCompletion(store) && ok;
    ok = expect(probeStatus == SecretStoreStatus::Success, "availability probe failed") && ok;
    ok = expect(store.availabilityStatus() == SecretStoreStatus::Success, "availability state was not updated") && ok;
    return ok;
  }

  bool cancellationAndDestruction() {
    bool ok = true;
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
      ok = expect(fake->waitForCalls(1), "blocked backend was not entered") && ok;
      operation.cancel();
      ok = dispatchCompletion(store) && ok;
      ok = expect(operation.cancelled(), "operation did not retain cancelled state") && ok;
      ok = expect(!callbackCalled, "cancelled operation invoked its callback") && ok;
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
      ok = expect(fake->waitForCalls(1), "destruction test backend was not entered") && ok;
    }
    ok = expect(!callbackCalled, "SecretStore destruction invoked a pending callback") && ok;
    return ok;
  }

  bool failureLogsExcludeSecretMaterial() {
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    fake->force(SecretStoreStatus::BackendError, SecretStoreErrorCategory::Protocol);
    security::SecretStore store(std::move(backend));
    const std::string secretMarker = "secret-value-marker-4815162342";
    const std::string labelMarker = "secret-label-marker-108";
    const auto markerBytes = std::span(reinterpret_cast<const std::uint8_t*>(secretMarker.data()), secretMarker.size());
    store.store(
        {.scope = "calendar", .owner = "logging-test", .name = "password"},
        security::SecureBuffer(markerBytes), labelMarker, [](SecretStoreStatus) {}
    );

    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    if (!expect(::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000) == 1, "log test completion timed out")) {
      return false;
    }

    std::array<int, 2> pipeFds{-1, -1};
    if (!expect(::pipe(pipeFds.data()) == 0, "failed to create stderr capture pipe")) {
      return false;
    }
    std::fflush(stderr);
    const int savedStderr = ::dup(STDERR_FILENO);
    if (!expect(savedStderr >= 0 && ::dup2(pipeFds[1], STDERR_FILENO) >= 0, "failed to redirect stderr")) {
      ::close(pipeFds[0]);
      ::close(pipeFds[1]);
      if (savedStderr >= 0) {
        ::close(savedStderr);
      }
      return false;
    }

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

    bool ok = true;
    ok = expect(captured.contains("backend-error"), "sanitized backend category was not logged") && ok;
    ok = expect(!captured.contains(secretMarker), "secret bytes appeared in logs") && ok;
    ok = expect(!captured.contains(labelMarker), "secret label appeared in logs") && ok;
    return ok;
  }
} // namespace

int main() {
  bool ok = true;
  ok = exactIdentityAndValidation() && ok;
  ok = binaryReplacementAndDeletion() && ok;
  ok = statusAndThreadDispatch() && ok;
  ok = cancellationAndDestruction() && ok;
  ok = failureLogsExcludeSecretMaterial() && ok;
  return ok ? 0 : 1;
}
