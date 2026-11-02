#pragma once

#include "security/encrypted_file_store.h"
#include "test_check.h"

#include <array>
#include <nlohmann/json.hpp>
#include <utility>

inline security::SecureKey notificationHistoryTestKey(std::uint8_t value = 0x42) {
  TEST_CHECK(security::initializeSecurityPrimitives());
  std::array<std::uint8_t, security::SecureKey::Size> bytes{};
  bytes.fill(value);
  auto key = security::SecureKey::fromBytes(bytes);
  TEST_CHECK(key.has_value());
  return std::move(*key);
}

inline nlohmann::json readNotificationHistoryJson(const std::filesystem::path& path, const security::SecureKey& key) {
  const auto result = security::readEncryptedFile(path, key, {"notification-history-v1", "history"}, 16 * 1024 * 1024);
  TEST_CHECK(result.succeeded());
  return nlohmann::json::parse(result.plaintext);
}
