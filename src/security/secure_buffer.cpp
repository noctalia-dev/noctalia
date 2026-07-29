#include "security/secure_buffer.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sodium.h>
#include <utility>

namespace security {
  namespace {
    static_assert(SecureKey::Size == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    std::once_flag g_sodiumInitOnce;
    std::atomic<bool> g_sodiumReady{false};
  } // namespace

  bool initializeSecurityPrimitives() {
    std::call_once(g_sodiumInitOnce, [] { g_sodiumReady.store(sodium_init() >= 0); });
    return g_sodiumReady.load();
  }

  bool securityPrimitivesReady() { return g_sodiumReady.load(); }

  SecureBuffer::SecureBuffer(std::size_t size)
      : m_data(size > 0 ? std::make_unique<std::uint8_t[]>(size) : nullptr), m_size(size) {}

  SecureBuffer::SecureBuffer(std::span<const std::uint8_t> bytes) : SecureBuffer(bytes.size()) {
    std::ranges::copy(bytes, m_data.get());
  }

  SecureBuffer::~SecureBuffer() { clear(); }

  SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
      : m_data(std::move(other.m_data)), m_size(std::exchange(other.m_size, 0)) {}

  SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) {
      clear();
      m_data = std::move(other.m_data);
      m_size = std::exchange(other.m_size, 0);
    }
    return *this;
  }

  std::span<std::uint8_t> SecureBuffer::bytes() { return {m_data.get(), m_size}; }

  std::span<const std::uint8_t> SecureBuffer::bytes() const { return {m_data.get(), m_size}; }

  std::size_t SecureBuffer::size() const { return m_size; }

  bool SecureBuffer::empty() const { return m_size == 0; }

  void SecureBuffer::clear() {
    if (m_data != nullptr) {
      sodium_memzero(m_data.get(), m_size);
      m_data.reset();
      m_size = 0;
    }
  }

  SecureKey::SecureKey(SecureBuffer buffer) : m_buffer(std::move(buffer)) {}

  std::optional<SecureKey> SecureKey::fromBuffer(SecureBuffer buffer) {
    if (buffer.size() != Size) {
      return std::nullopt;
    }
    return SecureKey(std::move(buffer));
  }

  std::optional<SecureKey> SecureKey::fromBytes(std::span<const std::uint8_t> bytes) {
    return fromBuffer(SecureBuffer(bytes));
  }

  std::optional<SecureKey> SecureKey::generate() {
    if (!securityPrimitivesReady()) {
      return std::nullopt;
    }
    SecureBuffer buffer(Size);
    randombytes_buf(buffer.bytes().data(), buffer.size());
    return SecureKey(std::move(buffer));
  }

  std::span<const std::uint8_t, SecureKey::Size> SecureKey::bytes() const {
    return std::span<const std::uint8_t, Size>(m_buffer.bytes().data(), Size);
  }

} // namespace security
