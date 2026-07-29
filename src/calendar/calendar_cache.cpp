#include "calendar/calendar_cache.h"

#include "util/file_utils.h"

#include <fstream>
#include <limits>

namespace calendar::cache {
  namespace {
    const security::EncryptionContext kEncryptionContext{
        .purpose = "calendar-events-v1",
        .objectId = "events",
    };
  }

  bool secureExisting(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path.parent_path(), ec)
        && !FileUtils::setPrivateDirectoryPermissions(path.parent_path(), ec)) {
      return false;
    }
    if (ec) {
      return false;
    }
    if (std::filesystem::exists(path, ec) && !FileUtils::setPrivateFilePermissions(path, ec)) {
      return false;
    }
    return !ec;
  }

  LegacyReadResult readLegacy(const std::filesystem::path& path, std::size_t maxBytes) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
      return {
          .status = ec == std::errc::no_such_file_or_directory ? LegacyReadStatus::NotFound : LegacyReadStatus::IoError,
      };
    }
    if (!std::filesystem::exists(status)) {
      return {.status = LegacyReadStatus::NotFound};
    }
    if (!std::filesystem::is_regular_file(status)) {
      return {.status = LegacyReadStatus::IoError};
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
      return {.status = LegacyReadStatus::IoError};
    }
    if (size > maxBytes || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
      return {.status = LegacyReadStatus::Oversized};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      return {.status = LegacyReadStatus::IoError};
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    if (!contents.empty()) {
      input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
      if (!input.good()) {
        return {.status = LegacyReadStatus::IoError};
      }
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      return {.status = LegacyReadStatus::IoError};
    }
    return {.status = LegacyReadStatus::Success, .contents = std::move(contents)};
  }

  security::EncryptedReadResult
  readEncrypted(const std::filesystem::path& path, const security::SecureKey& key, std::size_t maxPlaintextBytes) {
    return security::readEncryptedFile(path, key, kEncryptionContext, maxPlaintextBytes);
  }

  bool writeEncrypted(const std::filesystem::path& path, std::string_view content, const security::SecureKey& key) {
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    return security::writeEncryptedFile(path, bytes, key, kEncryptionContext);
  }
} // namespace calendar::cache
