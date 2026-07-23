#pragma once

#include "security/secure_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace security {

  // V1 envelope: "NOCTALIAENC1" | version | algorithm | nonce | ciphertext and tag.
  inline constexpr std::array<std::uint8_t, 12> EncryptedFileMagic = {
      'N', 'O', 'C', 'T', 'A', 'L', 'I', 'A', 'E', 'N', 'C', '1',
  };
  inline constexpr std::uint8_t EncryptedFileFormatVersion = 1;
  inline constexpr std::uint8_t EncryptedFileAlgorithmXChaCha20Poly1305 = 1;
  inline constexpr std::size_t EncryptedFileNonceSize = 24;
  inline constexpr std::size_t EncryptedFileTagSize = 16;
  inline constexpr std::size_t EncryptedFileHeaderSize = EncryptedFileMagic.size() + 2 + EncryptedFileNonceSize;

  struct EncryptionContext {
    std::string purpose;
    std::string objectId;
  };

  enum class EncryptedReadStatus {
    Success,
    NotFound,
    IoError,
    InvalidHeader,
    UnsupportedVersion,
    UnsupportedAlgorithm,
    Oversized,
    Truncated,
    AuthenticationFailed,
  };

  struct EncryptedReadResult {
    EncryptedReadStatus status = EncryptedReadStatus::IoError;
    std::vector<std::uint8_t> plaintext;

    [[nodiscard]] bool succeeded() const { return status == EncryptedReadStatus::Success; }
  };

  [[nodiscard]] bool writeEncryptedFile(
      const std::filesystem::path& path, std::span<const std::uint8_t> plaintext, const SecureKey& key,
      const EncryptionContext& context
  );

  [[nodiscard]] EncryptedReadResult readEncryptedFile(
      const std::filesystem::path& path, const SecureKey& key, const EncryptionContext& context,
      std::size_t maxPlaintextBytes
  );

} // namespace security
