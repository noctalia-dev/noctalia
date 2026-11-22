#include "calendar/calendar_cache.h"
#include "security/secure_buffer.h"
#include "tests/test_check.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
  constexpr std::filesystem::perms permissionMask() {
    using P = std::filesystem::perms;
    return P::owner_read
        | P::owner_write
        | P::owner_exec
        | P::group_read
        | P::group_write
        | P::group_exec
        | P::others_read
        | P::others_write
        | P::others_exec;
  }

  constexpr std::filesystem::perms privateFileMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  constexpr std::filesystem::perms privateDirectoryMode() {
    return privateFileMode() | std::filesystem::perms::owner_exec;
  }

  std::filesystem::path g_root;

  void removeRoot() {
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
  }

  std::filesystem::perms mode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & permissionMask();
  }
} // namespace

int main() {
  namespace fs = std::filesystem;
  using P = fs::perms;

  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  g_root = fs::temp_directory_path() / ("noctalia-calendar-cache-test-" + std::to_string(serial));
  const fs::path& root = g_root;
  const fs::path cacheDir = root / "noctalia/calendar";
  const fs::path legacyPath = cacheDir / "events.json";
  const fs::path encryptedPath = cacheDir / "events.enc";
  constexpr std::string_view plaintext = R"({"events":[{"title":"private appointment"}]})";

  TEST_CHECK(security::initializeSecurityPrimitives());
  std::array<std::uint8_t, security::SecureKey::Size> keyBytes{};
  std::array<std::uint8_t, security::SecureKey::Size> wrongKeyBytes{};
  keyBytes.fill(0x21);
  wrongKeyBytes.fill(0x22);
  auto key = security::SecureKey::fromBytes(keyBytes);
  auto wrongKey = security::SecureKey::fromBytes(wrongKeyBytes);
  TEST_CHECK(key.has_value() && wrongKey.has_value());

  fs::remove_all(root);
  fs::create_directories(cacheDir);
  std::atexit(removeRoot);
  {
    std::ofstream out(legacyPath, std::ios::trunc);
    out << plaintext;
  }
  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  fs::permissions(legacyPath, privateFileMode() | P::group_read | P::others_read, fs::perm_options::replace);

  TEST_CHECK(calendar::cache::secureExisting(legacyPath));
  TEST_CHECK(mode(cacheDir) == privateDirectoryMode());
  TEST_CHECK(mode(legacyPath) == privateFileMode());
  const auto legacy = calendar::cache::readLegacy(legacyPath, 1024);
  TEST_CHECK(legacy.status == calendar::cache::LegacyReadStatus::Success);
  const std::string legacyText(reinterpret_cast<const char*>(legacy.contents.data()), legacy.contents.size());
  TEST_CHECK(legacyText == plaintext);
  TEST_CHECK(
      calendar::cache::readLegacy(cacheDir / "missing.json", 1024).status == calendar::cache::LegacyReadStatus::NotFound
  );

  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  TEST_CHECK(calendar::cache::writeEncrypted(encryptedPath, plaintext, *key));
  TEST_CHECK(mode(cacheDir) == privateDirectoryMode());
  TEST_CHECK(mode(encryptedPath) == privateFileMode());

  {
    std::ifstream input(encryptedPath, std::ios::binary);
    const std::string envelope{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    TEST_CHECK(!envelope.contains("private appointment"));
  }
  const auto decrypted = calendar::cache::readEncrypted(encryptedPath, *key, 1024);
  const std::string decryptedText(
      reinterpret_cast<const char*>(decrypted.plaintext.data()), decrypted.plaintext.size()
  );
  TEST_CHECK(decrypted.succeeded() && decryptedText == plaintext);
  TEST_CHECK(
      calendar::cache::readEncrypted(encryptedPath, *wrongKey, 1024).status
      == security::EncryptedReadStatus::AuthenticationFailed
  );

  return 0;
}
