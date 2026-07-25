#include "security/storage_key_provider.h"

#include "core/log.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace security {
  namespace {
    constexpr Logger kLog("storage-key");
    constexpr SecretId kStorageMasterKeyId{
        .scope = "storage",
        .owner = "encrypted-state",
        .name = "master-key",
    };

    constexpr std::array<char, crypto_kdf_CONTEXTBYTES> kClipboardContext = {
        'N', 'O', 'C', 'T', 'C', 'L', 'I', 'P',
    };
    constexpr std::array<char, crypto_kdf_CONTEXTBYTES> kCalendarContext = {
        'N', 'O', 'C', 'T', 'C', 'A', 'L', 'E',
    };

    std::optional<std::uint8_t> decodeLowerHex(char value) {
      if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
      }
      if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(10 + value - 'a');
      }
      return std::nullopt;
    }
  } // namespace

  StorageKeyProvider::StorageKeyProvider(SecretStore& secretStore) : m_secretStore(secretStore) {}

  StorageKeyProvider::~StorageKeyProvider() { m_operation.cancel(); }

  void StorageKeyProvider::configure(StorageKeySource source, std::string keyFile, bool encryptedDataExists) {
    if (m_configured && m_source == source && m_keyFile == keyFile) {
      m_encryptedDataExists = m_encryptedDataExists || encryptedDataExists;
      return;
    }

    m_operation.cancel();
    finishReset(false);
    m_masterKey.reset();
    m_source = source;
    m_keyFile = std::move(keyFile);
    m_encryptedDataExists = encryptedDataExists;
    m_configured = true;
    setState(StorageKeyState::Opening);

    if (!securityPrimitivesReady()) {
      setState(StorageKeyState::BackendError);
      return;
    }
    if (m_source == StorageKeySource::File) {
      loadKeyFile();
    } else {
      lookupKey();
    }
  }

  void StorageKeyProvider::retry() {
    m_operation.cancel();
    finishReset(false);
    m_masterKey.reset();
    setState(StorageKeyState::Opening);
    if (!securityPrimitivesReady()) {
      setState(StorageKeyState::BackendError);
      return;
    }
    if (m_source == StorageKeySource::File) {
      loadKeyFile();
      return;
    }
    m_operation = m_secretStore.retryAvailabilityCheck([this](SecretStoreStatus status) {
      if (status == SecretStoreStatus::Success) {
        lookupKey();
      } else {
        setState(stateForSecretStoreStatus(status));
      }
    });
  }

  void StorageKeyProvider::resetAfterEncryptedDataCleared(ResetCallback callback) {
    m_operation.cancel();
    finishReset(false);
    m_resetCallback = std::move(callback);
    m_masterKey.reset();
    m_encryptedDataExists = false;
    setState(StorageKeyState::Opening);

    if (!securityPrimitivesReady()) {
      setState(StorageKeyState::BackendError);
      finishReset(false);
      return;
    }
    if (m_source == StorageKeySource::File) {
      loadKeyFile();
      return;
    }

    m_operation = m_secretStore.erase(kStorageMasterKeyId, [this](SecretStoreStatus status) {
      if (status == SecretStoreStatus::Success || status == SecretStoreStatus::NotFound) {
        createKey();
        return;
      }
      setState(stateForSecretStoreStatus(status));
      finishReset(false);
    });
  }

  void StorageKeyProvider::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  std::optional<SecureKey> StorageKeyProvider::deriveKey(StorageKeyPurpose purpose) const {
    if (!m_masterKey.has_value() || m_state != StorageKeyState::Ready || !securityPrimitivesReady()) {
      return std::nullopt;
    }

    const auto& context = purpose == StorageKeyPurpose::ClipboardHistory ? kClipboardContext : kCalendarContext;
    SecureBuffer derived(SecureKey::Size);
    if (crypto_kdf_derive_from_key(
            derived.bytes().data(), derived.size(), 1, context.data(), m_masterKey->bytes().data()
        )
        != 0) {
      return std::nullopt;
    }
    return SecureKey::fromBuffer(std::move(derived));
  }

  void StorageKeyProvider::loadKeyFile() {
    if (m_keyFile.empty()) {
      kLog.warn("file key source has no configured key file");
      setState(StorageKeyState::BackendError);
      return;
    }

    const int fd = ::open(m_keyFile.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      const int error = errno;
      const StorageKeyState state = error == ENOENT || error == ENOTDIR ? StorageKeyState::MissingKey
          : error == EACCES || error == EPERM                           ? StorageKeyState::DeniedOrLocked
                                                                        : StorageKeyState::BackendError;
      kLog.warn("failed to open configured storage key file: {}", std::strerror(error));
      setState(state);
      return;
    }

    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || (info.st_size != 64 && info.st_size != 65)) {
      ::close(fd);
      kLog.warn("storage key file must contain one 64-character lowercase hexadecimal key");
      setState(StorageKeyState::BackendError);
      return;
    }

    const auto fileSize = static_cast<std::size_t>(info.st_size);
    SecureBuffer encoded(fileSize);
    std::size_t offset = 0;
    while (offset < fileSize) {
      const ssize_t count = ::read(fd, encoded.bytes().data() + offset, fileSize - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      ::close(fd);
      kLog.warn("failed to read configured storage key file");
      setState(StorageKeyState::BackendError);
      return;
    }
    ::close(fd);

    if (fileSize == 65 && encoded.bytes().back() != static_cast<std::uint8_t>('\n')) {
      kLog.warn("storage key file has invalid trailing data");
      setState(StorageKeyState::BackendError);
      return;
    }

    SecureBuffer keyBytes(SecureKey::Size);
    for (std::size_t i = 0; i < SecureKey::Size; ++i) {
      const auto high = decodeLowerHex(static_cast<char>(encoded.bytes()[i * 2]));
      const auto low = decodeLowerHex(static_cast<char>(encoded.bytes()[i * 2 + 1]));
      if (!high.has_value() || !low.has_value()) {
        kLog.warn("storage key file must contain lowercase hexadecimal characters");
        setState(StorageKeyState::BackendError);
        return;
      }
      keyBytes.bytes()[i] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }

    auto key = SecureKey::fromBuffer(std::move(keyBytes));
    if (!key.has_value()) {
      setState(StorageKeyState::BackendError);
      return;
    }
    activateKey(std::move(*key));
  }

  void StorageKeyProvider::lookupKey() {
    setState(StorageKeyState::Opening);
    m_operation = m_secretStore.lookup(kStorageMasterKeyId, [this](SecretStoreStatus status, SecureBuffer value) {
      if (status == SecretStoreStatus::Success) {
        auto key = SecureKey::fromBuffer(std::move(value));
        if (!key.has_value()) {
          kLog.warn("storage master key has an invalid size");
          setState(StorageKeyState::BackendError);
          return;
        }
        activateKey(std::move(*key));
        return;
      }
      if (status == SecretStoreStatus::NotFound) {
        if (m_encryptedDataExists) {
          kLog.warn("encrypted state exists, but the storage master key is missing");
          setState(StorageKeyState::MissingKey);
        } else {
          createKey();
        }
        return;
      }
      setState(stateForSecretStoreStatus(status));
    });
  }

  void StorageKeyProvider::createKey() {
    auto generated = SecureKey::generate();
    if (!generated.has_value()) {
      kLog.warn("failed to generate storage master key");
      setState(StorageKeyState::BackendError);
      return;
    }

    auto key = std::make_shared<SecureKey>(std::move(*generated));
    SecureBuffer storedValue(key->bytes());
    m_operation = m_secretStore.store(
        kStorageMasterKeyId, std::move(storedValue), "Noctalia encrypted storage key",
        [this, key = std::move(key)](SecretStoreStatus status) mutable {
          if (status == SecretStoreStatus::Success) {
            activateKey(std::move(*key));
          } else {
            setState(stateForSecretStoreStatus(status));
          }
        }
    );
  }

  void StorageKeyProvider::activateKey(SecureKey key) {
    m_masterKey = std::move(key);
    setState(StorageKeyState::Ready, true);
    finishReset(true);
  }

  void StorageKeyProvider::setState(StorageKeyState state, bool forceNotify) {
    if (!forceNotify && m_state == state) {
      return;
    }
    m_state = state;
    if (m_changeCallback) {
      m_changeCallback();
    }
    if (state != StorageKeyState::Opening && state != StorageKeyState::Ready) {
      finishReset(false);
    }
  }

  void StorageKeyProvider::finishReset(bool success) {
    if (!m_resetCallback) {
      return;
    }
    auto callback = std::move(m_resetCallback);
    callback(success);
  }

  StorageKeyState StorageKeyProvider::stateForSecretStoreStatus(SecretStoreStatus status) {
    switch (status) {
    case SecretStoreStatus::Success:
      return StorageKeyState::Ready;
    case SecretStoreStatus::NotFound:
      return StorageKeyState::MissingKey;
    case SecretStoreStatus::Unavailable:
      return StorageKeyState::Unavailable;
    case SecretStoreStatus::Cancelled:
      return StorageKeyState::Cancelled;
    case SecretStoreStatus::DeniedOrLocked:
      return StorageKeyState::DeniedOrLocked;
    case SecretStoreStatus::BackendError:
      return StorageKeyState::BackendError;
    }
    return StorageKeyState::BackendError;
  }

} // namespace security
