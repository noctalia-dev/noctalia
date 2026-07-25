#include "calendar/calendar_cache.h"
#include "security/secure_buffer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
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

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "calendar_cache_permissions_test: {}", message);
    }
    return condition;
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
  const fs::path root = fs::temp_directory_path() / ("noctalia-calendar-cache-test-" + std::to_string(serial));
  const fs::path cacheDir = root / "noctalia/calendar";
  const fs::path legacyPath = cacheDir / "events.json";
  const fs::path encryptedPath = cacheDir / "events.enc";
  constexpr std::string_view plaintext = R"({"events":[{"title":"private appointment"}]})";

  if (!expect(security::initializeSecurityPrimitives(), "failed to initialize libsodium")) {
    return 1;
  }
  std::array<std::uint8_t, security::SecureKey::Size> keyBytes{};
  std::array<std::uint8_t, security::SecureKey::Size> wrongKeyBytes{};
  keyBytes.fill(0x21);
  wrongKeyBytes.fill(0x22);
  auto key = security::SecureKey::fromBytes(keyBytes);
  auto wrongKey = security::SecureKey::fromBytes(wrongKeyBytes);
  if (!expect(key.has_value() && wrongKey.has_value(), "failed to construct cache keys")) {
    return 1;
  }

  fs::remove_all(root);
  fs::create_directories(cacheDir);
  {
    std::ofstream out(legacyPath, std::ios::trunc);
    out << plaintext;
  }
  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  fs::permissions(legacyPath, privateFileMode() | P::group_read | P::others_read, fs::perm_options::replace);

  bool ok = true;
  ok = expect(calendar::cache::secureExisting(legacyPath), "failed to secure existing cache") && ok;
  ok = expect(mode(cacheDir) == privateDirectoryMode(), "existing cache directory mode was not 0700") && ok;
  ok = expect(mode(legacyPath) == privateFileMode(), "existing cache file mode was not 0600") && ok;
  const auto legacy = calendar::cache::readLegacy(legacyPath, 1024);
  ok = expect(legacy.status == calendar::cache::LegacyReadStatus::Success, "failed to read bounded legacy cache") && ok;
  const std::string legacyText(reinterpret_cast<const char*>(legacy.contents.data()), legacy.contents.size());
  ok = expect(legacyText == plaintext, "legacy cache contents changed") && ok;
  ok = expect(
           calendar::cache::readLegacy(cacheDir / "missing.json", 1024).status
               == calendar::cache::LegacyReadStatus::NotFound,
           "missing legacy cache was not reported as absent"
       )
      && ok;

  fs::permissions(
      cacheDir, P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec,
      fs::perm_options::replace
  );
  ok = expect(calendar::cache::writeEncrypted(encryptedPath, plaintext, *key), "failed to write encrypted cache") && ok;
  ok = expect(mode(cacheDir) == privateDirectoryMode(), "encrypted cache directory mode was not 0700") && ok;
  ok = expect(mode(encryptedPath) == privateFileMode(), "encrypted cache file mode was not 0600") && ok;

  {
    std::ifstream input(encryptedPath, std::ios::binary);
    const std::string envelope{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ok = expect(!envelope.contains("private appointment"), "encrypted cache exposed event plaintext") && ok;
  }
  const auto decrypted = calendar::cache::readEncrypted(encryptedPath, *key, 1024);
  const std::string decryptedText(
      reinterpret_cast<const char*>(decrypted.plaintext.data()), decrypted.plaintext.size()
  );
  ok = expect(decrypted.succeeded() && decryptedText == plaintext, "encrypted cache did not round-trip") && ok;
  ok = expect(
           calendar::cache::readEncrypted(encryptedPath, *wrongKey, 1024).status
               == security::EncryptedReadStatus::AuthenticationFailed,
           "wrong calendar cache key did not fail authentication"
       )
      && ok;

  fs::remove_all(root);
  return ok ? 0 : 1;
}
