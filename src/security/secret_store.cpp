#include "security/secret_store.h"

#include "core/log.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <libsecret/secret.h>
#include <limits>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace security {
  namespace {
    constexpr Logger kLog("secret-store");
    constexpr auto kRepeatedErrorLogInterval = std::chrono::seconds(30);

    enum class OperationKind { Probe, Lookup, Store, Erase };

    const char* operationName(OperationKind kind) {
      switch (kind) {
      case OperationKind::Probe:
        return "probe";
      case OperationKind::Lookup:
        return "lookup";
      case OperationKind::Store:
        return "store";
      case OperationKind::Erase:
        return "erase";
      }
      return "unknown";
    }

    const char* statusName(SecretStoreStatus status) {
      switch (status) {
      case SecretStoreStatus::Success:
        return "success";
      case SecretStoreStatus::NotFound:
        return "not-found";
      case SecretStoreStatus::Unavailable:
        return "unavailable";
      case SecretStoreStatus::Cancelled:
        return "cancelled";
      case SecretStoreStatus::DeniedOrLocked:
        return "denied-or-locked";
      case SecretStoreStatus::BackendError:
        return "backend-error";
      }
      return "unknown";
    }

    const char* categoryName(SecretStoreErrorCategory category) {
      switch (category) {
      case SecretStoreErrorCategory::None:
        return "none";
      case SecretStoreErrorCategory::ProviderUnavailable:
        return "provider-unavailable";
      case SecretStoreErrorCategory::Cancelled:
        return "cancelled";
      case SecretStoreErrorCategory::Locked:
        return "locked";
      case SecretStoreErrorCategory::PermissionDenied:
        return "permission-denied";
      case SecretStoreErrorCategory::Protocol:
        return "protocol";
      case SecretStoreErrorCategory::InvalidRequest:
        return "invalid-request";
      case SecretStoreErrorCategory::Other:
        return "other";
      }
      return "unknown";
    }

    SecretStoreBackendResult cancelledResult() {
      return {
          .status = SecretStoreStatus::Cancelled,
          .errorCategory = SecretStoreErrorCategory::Cancelled,
      };
    }

    SecretStoreBackendResult invalidRequestResult() {
      return {
          .status = SecretStoreStatus::BackendError,
          .errorCategory = SecretStoreErrorCategory::InvalidRequest,
      };
    }

    const SecretSchema& schema() {
      static const SecretSchema value = {
          SecretStoreSchemaName.data(),
          SECRET_SCHEMA_NONE,
          {
              {"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"scope", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"owner", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"name", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {"version", SECRET_SCHEMA_ATTRIBUTE_STRING},
              {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
          },
      };
      return value;
    }

    using HashTablePtr = std::unique_ptr<GHashTable, decltype(&g_hash_table_unref)>;
    using SecretValuePtr = std::unique_ptr<SecretValue, decltype(&secret_value_unref)>;
    using ErrorPtr = std::unique_ptr<GError, decltype(&g_error_free)>;
    using CancellablePtr = std::shared_ptr<GCancellable>;

    HashTablePtr makeAttributes(const SecretStoreAttributes& attributes) {
      HashTablePtr table(g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free), &g_hash_table_unref);
      const auto insert = [&table](const char* key, const std::string& value) {
        g_hash_table_insert(table.get(), g_strdup(key), g_strdup(value.c_str()));
      };
      insert("application", attributes.application);
      insert("scope", attributes.scope);
      insert("owner", attributes.owner);
      insert("name", attributes.name);
      insert("version", attributes.version);
      return table;
    }

    bool isRemoteError(const GError& error, const char* expected) {
      if (!g_dbus_error_is_remote_error(&error)) {
        return false;
      }
      std::unique_ptr<char, decltype(&g_free)> remote(g_dbus_error_get_remote_error(&error), &g_free);
      return remote != nullptr && std::string_view(remote.get()) == expected;
    }

    SecretStoreBackendResult resultFromError(const GError* error) {
      if (error == nullptr) {
        return {
            .status = SecretStoreStatus::BackendError,
            .errorCategory = SecretStoreErrorCategory::Other,
        };
      }
      if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        return cancelledResult();
      }
      if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)
          || isRemoteError(*error, "org.freedesktop.Secret.Error.IsLocked")) {
        return {
            .status = SecretStoreStatus::DeniedOrLocked,
            .errorCategory = SecretStoreErrorCategory::Locked,
        };
      }
      if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)
          || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED)
          || isRemoteError(*error, "org.freedesktop.DBus.Error.AccessDenied")) {
        return {
            .status = SecretStoreStatus::DeniedOrLocked,
            .errorCategory = SecretStoreErrorCategory::PermissionDenied,
        };
      }
      if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
          || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)
          || g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND)
          || isRemoteError(*error, "org.freedesktop.DBus.Error.ServiceUnknown")
          || isRemoteError(*error, "org.freedesktop.DBus.Error.NameHasNoOwner")
          || isRemoteError(*error, "org.freedesktop.DBus.Error.Spawn.ServiceNotFound")) {
        return {
            .status = SecretStoreStatus::Unavailable,
            .errorCategory = SecretStoreErrorCategory::ProviderUnavailable,
        };
      }
      if (error->domain == SECRET_ERROR) {
        return {
            .status = SecretStoreStatus::BackendError,
            .errorCategory = SecretStoreErrorCategory::Protocol,
        };
      }
      return {
          .status = SecretStoreStatus::BackendError,
          .errorCategory = SecretStoreErrorCategory::Other,
      };
    }

    template <typename Function>
    SecretStoreBackendResult withCancellable(SecretStoreCancellation& cancellation, Function&& function) {
      if (cancellation.cancelled()) {
        return cancelledResult();
      }

      CancellablePtr native(g_cancellable_new(), [](GCancellable* value) { g_object_unref(value); });
      cancellation.setCancelCallback([native]() { g_cancellable_cancel(native.get()); });
      SecretStoreBackendResult result = function(native.get());
      cancellation.clearCancelCallback();
      return result;
    }

    class LibsecretBackend final : public SecretStoreBackend {
    public:
      SecretStoreBackendResult probe(SecretStoreCancellation& cancellation) override {
        return withCancellable(cancellation, [](GCancellable* cancellable) {
          GError* rawError = nullptr;
          SecretService* service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, cancellable, &rawError);
          ErrorPtr error(rawError, &g_error_free);
          if (service == nullptr) {
            return resultFromError(error.get());
          }
          g_object_unref(service);
          return SecretStoreBackendResult{
              .status = SecretStoreStatus::Success,
              .errorCategory = SecretStoreErrorCategory::None,
          };
        });
      }

      SecretStoreBackendResult
      lookup(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) override {
        return withCancellable(cancellation, [&attributes](GCancellable* cancellable) {
          auto table = makeAttributes(attributes);
          GError* rawError = nullptr;
          SecretValuePtr value(
              secret_password_lookupv_binary_sync(&schema(), table.get(), cancellable, &rawError), &secret_value_unref
          );
          ErrorPtr error(rawError, &g_error_free);
          if (value == nullptr) {
            if (error != nullptr) {
              return resultFromError(error.get());
            }
            return SecretStoreBackendResult{
                .status = SecretStoreStatus::NotFound,
                .errorCategory = SecretStoreErrorCategory::None,
            };
          }

          gsize length = 0;
          const gchar* secret = secret_value_get(value.get(), &length);
          if (secret == nullptr) {
            return SecretStoreBackendResult{
                .status = SecretStoreStatus::BackendError,
                .errorCategory = SecretStoreErrorCategory::Protocol,
            };
          }
          const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(secret), length);
          return SecretStoreBackendResult{
              .status = SecretStoreStatus::Success,
              .errorCategory = SecretStoreErrorCategory::None,
              .value = SecureBuffer(bytes),
          };
        });
      }

      SecretStoreBackendResult store(
          const SecretStoreAttributes& attributes, std::span<const std::uint8_t> bytes, const std::string& label,
          SecretStoreCancellation& cancellation
      ) override {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<gssize>::max())) {
          return invalidRequestResult();
        }
        return withCancellable(cancellation, [&attributes, bytes, &label](GCancellable* cancellable) {
          auto table = makeAttributes(attributes);
          const char* data = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
          SecretValuePtr value(
              secret_value_new(data, static_cast<gssize>(bytes.size()), "application/octet-stream"), &secret_value_unref
          );
          GError* rawError = nullptr;
          const gboolean stored = secret_password_storev_binary_sync(
              &schema(), table.get(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.get(), cancellable, &rawError
          );
          ErrorPtr error(rawError, &g_error_free);
          if (stored != 0) {
            return SecretStoreBackendResult{
                .status = SecretStoreStatus::Success,
                .errorCategory = SecretStoreErrorCategory::None,
            };
          }
          return error != nullptr ? resultFromError(error.get()) : cancelledResult();
        });
      }

      SecretStoreBackendResult
      erase(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) override {
        return withCancellable(cancellation, [&attributes](GCancellable* cancellable) {
          auto table = makeAttributes(attributes);
          GError* rawError = nullptr;
          const gboolean erased = secret_password_clearv_sync(&schema(), table.get(), cancellable, &rawError);
          ErrorPtr error(rawError, &g_error_free);
          if (erased != 0) {
            return SecretStoreBackendResult{
                .status = SecretStoreStatus::Success,
                .errorCategory = SecretStoreErrorCategory::None,
            };
          }
          if (error != nullptr) {
            return resultFromError(error.get());
          }
          return SecretStoreBackendResult{
              .status = SecretStoreStatus::NotFound,
              .errorCategory = SecretStoreErrorCategory::None,
          };
        });
      }
    };

    std::unique_ptr<SecretStoreBackend> makeLibsecretBackend() { return std::make_unique<LibsecretBackend>(); }
  } // namespace

  SecretStoreAttributes secretStoreAttributes(const SecretId& id) {
    return {
        .application = "noctalia",
        .scope = id.scope,
        .owner = id.owner,
        .name = id.name,
        .version = "1",
    };
  }

  bool isValidSecretId(const SecretId& id) {
    if (id.owner.empty()) {
      return false;
    }
    if (id.scope == "calendar") {
      return id.name == "password" || id.name == "refresh-token";
    }
    if (id.scope == "storage") {
      return id.owner == "encrypted-state" && id.name == "master-key";
    }
    return false;
  }

  void SecretStoreCancellation::cancel() {
    std::function<void()> callback;
    {
      std::scoped_lock lock(m_mutex);
      if (m_cancelled) {
        return;
      }
      m_cancelled = true;
      callback = m_cancelCallback;
    }
    if (callback) {
      callback();
    }
  }

  bool SecretStoreCancellation::cancelled() const {
    std::scoped_lock lock(m_mutex);
    return m_cancelled;
  }

  void SecretStoreCancellation::setCancelCallback(std::function<void()> callback) {
    bool cancelNow = false;
    {
      std::scoped_lock lock(m_mutex);
      cancelNow = m_cancelled;
      if (!cancelNow) {
        m_cancelCallback = callback;
      }
    }
    if (cancelNow && callback) {
      callback();
    }
  }

  void SecretStoreCancellation::clearCancelCallback() {
    std::scoped_lock lock(m_mutex);
    m_cancelCallback = {};
  }

  SecretStoreOperation::SecretStoreOperation(std::weak_ptr<SecretStoreCancellation> cancellation)
      : m_cancellation(std::move(cancellation)) {}

  void SecretStoreOperation::cancel() const {
    if (auto cancellation = m_cancellation.lock()) {
      cancellation->cancel();
    }
  }

  bool SecretStoreOperation::cancelled() const {
    if (auto cancellation = m_cancellation.lock()) {
      return cancellation->cancelled();
    }
    return true;
  }

  class SecretStore::Impl {
  public:
    struct Request {
      OperationKind kind = OperationKind::Probe;
      SecretId id;
      SecretStoreAttributes attributes;
      SecureBuffer value;
      std::string label;
      LookupCallback lookupCallback;
      StatusCallback statusCallback;
      std::shared_ptr<SecretStoreCancellation> cancellation;
      bool valid = true;
    };

    struct Completion {
      OperationKind kind = OperationKind::Probe;
      SecretId id;
      SecretStoreBackendResult result;
      LookupCallback lookupCallback;
      StatusCallback statusCallback;
      std::shared_ptr<SecretStoreCancellation> cancellation;
    };

    explicit Impl(std::unique_ptr<SecretStoreBackend> backend) : m_backend(std::move(backend)) {
      if (m_backend == nullptr) {
        throw std::invalid_argument("SecretStore requires a backend");
      }
      m_eventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (m_eventFd < 0) {
        throw std::runtime_error("failed to create SecretStore eventfd");
      }
      m_worker = std::thread([this]() { workerLoop(); });
    }

    ~Impl() {
      std::shared_ptr<SecretStoreCancellation> active;
      {
        std::scoped_lock lock(m_queueMutex);
        m_shutdown = true;
        for (auto& request : m_requests) {
          request.cancellation->cancel();
        }
        m_requests.clear();
        active = m_activeCancellation;
      }
      if (active != nullptr) {
        active->cancel();
      }
      m_queueCv.notify_all();
      if (m_worker.joinable()) {
        m_worker.join();
      }
      {
        std::scoped_lock lock(m_completionMutex);
        m_completions.clear();
      }
      ::close(m_eventFd);
      m_eventFd = -1;
    }

    SecretStoreOperation enqueue(Request request) {
      auto cancellation = request.cancellation;
      {
        std::scoped_lock lock(m_queueMutex);
        if (m_shutdown) {
          cancellation->cancel();
          return SecretStoreOperation(cancellation);
        }
        m_requests.push_back(std::move(request));
      }
      m_queueCv.notify_one();
      return SecretStoreOperation(cancellation);
    }

    void addPollFd(std::vector<pollfd>& fds) const { fds.push_back({.fd = m_eventFd, .events = POLLIN, .revents = 0}); }

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
      if (startIdx >= fds.size() || (fds[startIdx].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
        return;
      }
      drainEventFd();

      std::deque<Completion> completions;
      {
        std::scoped_lock lock(m_completionMutex);
        completions = std::move(m_completions);
        m_completions.clear();
      }
      for (auto& completion : completions) {
        if (completion.cancellation->cancelled()) {
          continue;
        }
        updateAvailability(completion.result.status);
        logResult(completion);
        if (completion.kind == OperationKind::Lookup) {
          if (completion.lookupCallback) {
            completion.lookupCallback(completion.result.status, std::move(completion.result.value));
          }
        } else if (completion.statusCallback) {
          completion.statusCallback(completion.result.status);
        }
      }
    }

    [[nodiscard]] std::optional<SecretStoreStatus> availabilityStatus() const { return m_availabilityStatus; }

  private:
    void workerLoop() {
      while (true) {
        Request request;
        {
          std::unique_lock lock(m_queueMutex);
          m_queueCv.wait(lock, [this]() { return m_shutdown || !m_requests.empty(); });
          if (m_shutdown) {
            return;
          }
          request = std::move(m_requests.front());
          m_requests.pop_front();
          m_activeCancellation = request.cancellation;
        }

        SecretStoreBackendResult result;
        if (!request.valid) {
          result = invalidRequestResult();
        } else if (request.cancellation->cancelled()) {
          result = cancelledResult();
        } else {
          try {
            switch (request.kind) {
            case OperationKind::Probe:
              result = m_backend->probe(*request.cancellation);
              break;
            case OperationKind::Lookup:
              result = m_backend->lookup(request.attributes, *request.cancellation);
              break;
            case OperationKind::Store:
              result =
                  m_backend->store(request.attributes, request.value.bytes(), request.label, *request.cancellation);
              break;
            case OperationKind::Erase:
              result = m_backend->erase(request.attributes, *request.cancellation);
              break;
            }
          } catch (...) {
            result = {
                .status = SecretStoreStatus::BackendError,
                .errorCategory = SecretStoreErrorCategory::Other,
            };
          }
        }

        {
          std::scoped_lock lock(m_queueMutex);
          m_activeCancellation.reset();
          if (m_shutdown) {
            return;
          }
        }
        {
          std::scoped_lock lock(m_completionMutex);
          m_completions.push_back(
              Completion{
                  .kind = request.kind,
                  .id = std::move(request.id),
                  .result = std::move(result),
                  .lookupCallback = std::move(request.lookupCallback),
                  .statusCallback = std::move(request.statusCallback),
                  .cancellation = std::move(request.cancellation),
              }
          );
        }
        signalMain();
      }
    }

    void signalMain() const {
      constexpr std::uint64_t one = 1;
      const ssize_t written = ::write(m_eventFd, &one, sizeof(one));
      if (written < 0 && errno != EAGAIN) {
        kLog.warn("failed to signal SecretStore eventfd: errno={}", errno);
      }
    }

    void drainEventFd() const {
      std::uint64_t ignored = 0;
      while (::read(m_eventFd, &ignored, sizeof(ignored)) > 0) {
      }
    }

    void updateAvailability(SecretStoreStatus status) {
      m_availabilityStatus = status == SecretStoreStatus::NotFound ? SecretStoreStatus::Success : status;
    }

    void logResult(const Completion& completion) {
      const auto status = completion.result.status;
      if (status == SecretStoreStatus::Success || status == SecretStoreStatus::NotFound) {
        return;
      }
      if (status == SecretStoreStatus::Cancelled) {
        kLog.debug(
            "operation={} scope={} owner={} status={} category={}", operationName(completion.kind), completion.id.scope,
            completion.id.owner, statusName(status), categoryName(completion.result.errorCategory)
        );
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      const bool repeatedStatus = m_lastLoggedStatus.has_value() && *m_lastLoggedStatus == status;
      if (repeatedStatus && now - m_lastErrorLog < kRepeatedErrorLogInterval) {
        return;
      }
      m_lastLoggedStatus = status;
      m_lastErrorLog = now;
      kLog.warn(
          "operation={} scope={} owner={} status={} category={}", operationName(completion.kind), completion.id.scope,
          completion.id.owner, statusName(status), categoryName(completion.result.errorCategory)
      );
    }

    std::unique_ptr<SecretStoreBackend> m_backend;
    int m_eventFd = -1;
    std::thread m_worker;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Request> m_requests;
    std::shared_ptr<SecretStoreCancellation> m_activeCancellation;
    bool m_shutdown = false;

    std::mutex m_completionMutex;
    std::deque<Completion> m_completions;

    std::optional<SecretStoreStatus> m_availabilityStatus;
    std::optional<SecretStoreStatus> m_lastLoggedStatus;
    std::chrono::steady_clock::time_point m_lastErrorLog;
  };

  SecretStore::SecretStore() : SecretStore(makeLibsecretBackend()) {}

  SecretStore::SecretStore(std::unique_ptr<SecretStoreBackend> backend)
      : m_impl(std::make_unique<Impl>(std::move(backend))) {}

  SecretStore::~SecretStore() = default;

  SecretStoreOperation SecretStore::lookup(SecretId id, LookupCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Lookup,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .lookupCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation SecretStore::store(SecretId id, SecureBuffer value, std::string label, StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Store,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .value = std::move(value),
            .label = std::move(label),
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation SecretStore::erase(SecretId id, StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    const bool valid = isValidSecretId(id);
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Erase,
            .id = id,
            .attributes = secretStoreAttributes(id),
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
            .valid = valid,
        }
    );
  }

  SecretStoreOperation SecretStore::retryAvailabilityCheck(StatusCallback callback) {
    auto cancellation = std::make_shared<SecretStoreCancellation>();
    return m_impl->enqueue(
        Impl::Request{
            .kind = OperationKind::Probe,
            .statusCallback = std::move(callback),
            .cancellation = std::move(cancellation),
        }
    );
  }

  std::optional<SecretStoreStatus> SecretStore::availabilityStatus() const { return m_impl->availabilityStatus(); }

  void SecretStore::doAddPollFds(std::vector<pollfd>& fds) { m_impl->addPollFd(fds); }

  void SecretStore::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) { m_impl->dispatch(fds, startIdx); }

} // namespace security
