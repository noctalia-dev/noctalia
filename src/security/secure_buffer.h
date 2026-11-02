#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace security {

  [[nodiscard]] bool initializeSecurityPrimitives();
  bool securityPrimitivesReady();

  class SecureBuffer {
  public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size);
    explicit SecureBuffer(std::span<const std::uint8_t> bytes);
    ~SecureBuffer();

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept;
    SecureBuffer& operator=(SecureBuffer&& other) noexcept;

    std::span<std::uint8_t> bytes();
    std::span<const std::uint8_t> bytes() const;
    std::size_t size() const;
    bool empty() const;

    void clear();

  private:
    std::unique_ptr<std::uint8_t[]> m_data;
    std::size_t m_size = 0;
  };

  class SecureKey {
  public:
    static constexpr std::size_t Size = 32;

    SecureKey(const SecureKey&) = delete;
    SecureKey& operator=(const SecureKey&) = delete;
    SecureKey(SecureKey&&) noexcept = default;
    SecureKey& operator=(SecureKey&&) noexcept = default;

    [[nodiscard]] static std::optional<SecureKey> fromBuffer(SecureBuffer buffer);
    static std::optional<SecureKey> fromBytes(std::span<const std::uint8_t> bytes);
    static std::optional<SecureKey> generate();

    std::span<const std::uint8_t, Size> bytes() const;

  private:
    explicit SecureKey(SecureBuffer buffer);

    SecureBuffer m_buffer;
  };

} // namespace security
