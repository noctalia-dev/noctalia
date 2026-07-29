#pragma once

#include "app/poll_source.h"
#include "security/secure_buffer.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace security {

  inline constexpr std::string_view SecretStoreSchemaName = "dev.noctalia.Secret";

  struct SecretId {
    std::string scope;
    std::string owner;
    std::string name;

    bool operator==(const SecretId&) const = default;
  };

  struct SecretStoreAttributes {
    std::string application;
    std::string scope;
    std::string owner;
    std::string name;
    std::string version;

    bool operator==(const SecretStoreAttributes&) const = default;
  };

  [[nodiscard]] SecretStoreAttributes secretStoreAttributes(const SecretId& id);
  [[nodiscard]] bool isValidSecretId(const SecretId& id);

  enum class SecretStoreStatus {
    Success,
    NotFound,
    Unavailable,
    Cancelled,
    DeniedOrLocked,
    BackendError,
  };

  enum class SecretStoreErrorCategory {
    None,
    ProviderUnavailable,
    Cancelled,
    Locked,
    PermissionDenied,
    Protocol,
    InvalidRequest,
    Other,
  };

  struct SecretStoreBackendResult {
    SecretStoreStatus status = SecretStoreStatus::BackendError;
    SecretStoreErrorCategory errorCategory = SecretStoreErrorCategory::Other;
    SecureBuffer value;
  };

  class SecretStoreCancellation {
  public:
    void cancel();
    [[nodiscard]] bool cancelled() const;

    void setCancelCallback(std::function<void()> callback);
    void clearCancelCallback();

  private:
    mutable std::mutex m_mutex;
    bool m_cancelled = false;
    std::function<void()> m_cancelCallback;
  };

  class SecretStoreBackend {
  public:
    virtual ~SecretStoreBackend() = default;

    virtual SecretStoreBackendResult probe(SecretStoreCancellation& cancellation) = 0;
    virtual SecretStoreBackendResult
    lookup(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) = 0;
    virtual SecretStoreBackendResult store(
        const SecretStoreAttributes& attributes, std::span<const std::uint8_t> value, const std::string& label,
        SecretStoreCancellation& cancellation
    ) = 0;
    virtual SecretStoreBackendResult
    erase(const SecretStoreAttributes& attributes, SecretStoreCancellation& cancellation) = 0;
  };

  class SecretStoreOperation {
  public:
    SecretStoreOperation() = default;

    void cancel() const;
    [[nodiscard]] bool cancelled() const;

  private:
    friend class SecretStore;
    explicit SecretStoreOperation(std::weak_ptr<SecretStoreCancellation> cancellation);

    std::weak_ptr<SecretStoreCancellation> m_cancellation;
  };

  class SecretStore final : public PollSource {
  public:
    using LookupCallback = std::function<void(SecretStoreStatus, SecureBuffer)>;
    using StatusCallback = std::function<void(SecretStoreStatus)>;

    SecretStore();
    explicit SecretStore(std::unique_ptr<SecretStoreBackend> backend);
    ~SecretStore() override;

    SecretStore(const SecretStore&) = delete;
    SecretStore& operator=(const SecretStore&) = delete;

    SecretStoreOperation lookup(SecretId id, LookupCallback callback);
    SecretStoreOperation store(SecretId id, SecureBuffer value, std::string label, StatusCallback callback);
    SecretStoreOperation erase(SecretId id, StatusCallback callback);
    SecretStoreOperation retryAvailabilityCheck(StatusCallback callback = {});

    [[nodiscard]] std::optional<SecretStoreStatus> availabilityStatus() const;

    void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

  protected:
    void doAddPollFds(std::vector<pollfd>& fds) override;

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace security
