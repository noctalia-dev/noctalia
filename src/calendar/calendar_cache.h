#pragma once

#include "security/encrypted_file_store.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace calendar::cache {
  enum class LegacyReadStatus {
    Success,
    NotFound,
    IoError,
    Oversized,
  };

  struct [[nodiscard]] LegacyReadResult {
    LegacyReadStatus status = LegacyReadStatus::IoError;
    std::vector<std::uint8_t> contents;
  };

  [[nodiscard]] bool secureExisting(const std::filesystem::path& path);
  LegacyReadResult readLegacy(const std::filesystem::path& path, std::size_t maxBytes);
  security::EncryptedReadResult
  readEncrypted(const std::filesystem::path& path, const security::SecureKey& key, std::size_t maxPlaintextBytes);
  [[nodiscard]] bool
  writeEncrypted(const std::filesystem::path& path, std::string_view content, const security::SecureKey& key);
} // namespace calendar::cache
