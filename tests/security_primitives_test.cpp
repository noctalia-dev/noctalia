#include "security/encrypted_file_store.h"
#include "security/secure_buffer.h"
#include "util/file_utils.h"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using security::EncryptedReadStatus;

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "security_primitives_test: {}", message);
    }
    return condition;
  }

  class TestRoot {
  public:
    TestRoot() {
      const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
      m_path = std::filesystem::temp_directory_path() / ("noctalia-security-test-" + std::to_string(serial));
      std::filesystem::create_directories(m_path);
    }

    ~TestRoot() { std::filesystem::remove_all(m_path); }

    TestRoot(const TestRoot&) = delete;
    TestRoot& operator=(const TestRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

  private:
    std::filesystem::path m_path;
  };

  class UmaskGuard {
  public:
    explicit UmaskGuard(mode_t mode) : m_previous(::umask(mode)) {}
    ~UmaskGuard() { ::umask(m_previous); }

    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;

  private:
    mode_t m_previous;
  };

  std::optional<security::SecureKey> makeKey(std::uint8_t seed) {
    std::array<std::uint8_t, security::SecureKey::Size> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    return security::SecureKey::fromBytes(bytes);
  }

  std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  bool writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
  }

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

  std::filesystem::perms mode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & permissionMask();
  }

  bool secureBufferBehavior() {
    const std::array<std::uint8_t, 5> source = {1, 2, 0, 4, 5};
    security::SecureBuffer original(source);
    security::SecureBuffer moved(std::move(original));

    bool ok = true;
    ok = expect(original.empty(), "moved-from secure buffer was not empty") && ok;
    ok = expect(std::ranges::equal(moved.bytes(), source), "secure buffer move changed bytes") && ok;
    moved.clear();
    ok = expect(moved.empty(), "cleared secure buffer was not empty") && ok;

    security::SecureBuffer wrongSize(security::SecureKey::Size - 1);
    ok = expect(!security::SecureKey::fromBuffer(std::move(wrongSize)).has_value(), "accepted a short key") && ok;
    ok = expect(security::SecureKey::generate().has_value(), "failed to generate a secure key") && ok;
    return ok;
  }

  bool roundTrips() {
    TestRoot root;
    auto key = makeKey(7);
    if (!expect(key.has_value(), "failed to construct test key")) {
      return false;
    }
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "round-trip"};

    std::vector<std::vector<std::uint8_t>> fixtures;
    fixtures.emplace_back();
    fixtures.emplace_back(std::initializer_list<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
    fixtures.emplace_back(std::initializer_list<std::uint8_t>{0, 1, 2, 0, 255});
    fixtures.emplace_back(10U * 1024U * 1024U, 0x5a);

    bool ok = true;
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
      const auto path = root.path() / (std::to_string(index) + ".enc");
      ok = expect(security::writeEncryptedFile(path, fixtures[index], *key, context), "encrypted write failed") && ok;
      const auto result = security::readEncryptedFile(path, *key, context, fixtures[index].size());
      ok = expect(result.succeeded(), "encrypted read failed") && ok;
      ok = expect(result.plaintext == fixtures[index], "round-trip plaintext differed") && ok;
    }
    return ok;
  }

  bool noncesAndPlaintextHiding() {
    TestRoot root;
    auto key = makeKey(17);
    if (!expect(key.has_value(), "failed to construct nonce test key")) {
      return false;
    }
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "nonce"};
    const std::string secret = "distinct plaintext marker 90210";
    const auto plaintext = std::as_bytes(std::span(secret));
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
    const auto firstPath = root.path() / "first.enc";
    const auto secondPath = root.path() / "second.enc";

    bool ok = true;
    ok = expect(security::writeEncryptedFile(firstPath, bytes, *key, context), "first nonce write failed") && ok;
    ok = expect(security::writeEncryptedFile(secondPath, bytes, *key, context), "second nonce write failed") && ok;
    const auto first = readBytes(firstPath);
    const auto second = readBytes(secondPath);
    ok = expect(first != second, "fresh writes produced identical envelopes") && ok;
    const auto firstText = std::string_view(reinterpret_cast<const char*>(first.data()), first.size());
    ok = expect(!firstText.contains(secret), "plaintext marker appeared in ciphertext") && ok;
    return ok;
  }

  bool authenticationAndContextFailures() {
    TestRoot root;
    auto key = makeKey(27);
    auto wrongKey = makeKey(28);
    if (!expect(key.has_value() && wrongKey.has_value(), "failed to construct authentication test keys")) {
      return false;
    }
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "alpha"};
    const std::array<std::uint8_t, 4> plaintext = {4, 3, 2, 1};
    const auto path = root.path() / "auth.enc";

    bool ok = true;
    ok = expect(security::writeEncryptedFile(path, plaintext, *key, context), "authentication fixture write failed")
        && ok;
    ok = expect(
             security::readEncryptedFile(path, *wrongKey, context, 64).status
                 == EncryptedReadStatus::AuthenticationFailed,
             "wrong key did not fail authentication"
         )
        && ok;
    ok = expect(
             security::readEncryptedFile(path, *key, {.purpose = "other-purpose-v1", .objectId = "alpha"}, 64).status
                 == EncryptedReadStatus::AuthenticationFailed,
             "wrong purpose did not fail authentication"
         )
        && ok;
    ok = expect(
             security::readEncryptedFile(path, *key, {.purpose = "test-data-v1", .objectId = "beta"}, 64).status
                 == EncryptedReadStatus::AuthenticationFailed,
             "wrong object id did not fail authentication"
         )
        && ok;

    auto envelope = readBytes(path);
    envelope.back() ^= 0x80;
    ok = expect(writeBytes(path, envelope), "failed to modify authentication tag") && ok;
    const auto modified = security::readEncryptedFile(path, *key, context, 64);
    ok = expect(modified.status == EncryptedReadStatus::AuthenticationFailed, "modified tag was accepted") && ok;
    ok = expect(modified.plaintext.empty(), "authentication failure exposed plaintext") && ok;
    return ok;
  }

  bool parserFailures() {
    TestRoot root;
    auto key = makeKey(37);
    if (!expect(key.has_value(), "failed to construct parser test key")) {
      return false;
    }
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "parser"};
    const std::array<std::uint8_t, 6> plaintext = {1, 2, 3, 4, 5, 6};
    const auto canonicalPath = root.path() / "canonical.enc";
    if (!expect(security::writeEncryptedFile(canonicalPath, plaintext, *key, context), "parser fixture write failed")) {
      return false;
    }
    const auto canonical = readBytes(canonicalPath);
    bool ok = true;

    const auto expectMutation = [&](std::string_view name, std::vector<std::uint8_t> bytes,
                                    EncryptedReadStatus expected) {
      const auto path = root.path() / (std::string(name) + ".enc");
      return expect(writeBytes(path, bytes), "failed to write parser mutation")
          && expect(security::readEncryptedFile(path, *key, context, 64).status == expected, name);
    };

    auto invalidMagic = canonical;
    invalidMagic[0] ^= 1;
    ok = expectMutation("invalid magic", std::move(invalidMagic), EncryptedReadStatus::InvalidHeader) && ok;

    auto unsupportedVersion = canonical;
    unsupportedVersion[security::EncryptedFileMagic.size()] = 2;
    ok = expectMutation(
             "unsupported version", std::move(unsupportedVersion), EncryptedReadStatus::UnsupportedVersion
         )
        && ok;

    auto unsupportedAlgorithm = canonical;
    unsupportedAlgorithm[security::EncryptedFileMagic.size() + 1] = 2;
    ok = expectMutation(
             "unsupported algorithm", std::move(unsupportedAlgorithm), EncryptedReadStatus::UnsupportedAlgorithm
         )
        && ok;

    auto modifiedNonce = canonical;
    modifiedNonce[security::EncryptedFileMagic.size() + 2] ^= 1;
    ok = expectMutation(
             "modified nonce", std::move(modifiedNonce), EncryptedReadStatus::AuthenticationFailed
         )
        && ok;

    auto appended = canonical;
    appended.push_back(0);
    ok = expectMutation("appended garbage", std::move(appended), EncryptedReadStatus::AuthenticationFailed) && ok;

    auto truncatedHeader = canonical;
    truncatedHeader.resize(security::EncryptedFileHeaderSize - 1);
    ok = expectMutation("truncated header", std::move(truncatedHeader), EncryptedReadStatus::Truncated) && ok;

    auto truncatedTag = canonical;
    truncatedTag.resize(security::EncryptedFileHeaderSize + security::EncryptedFileTagSize - 1);
    ok = expectMutation("truncated tag", std::move(truncatedTag), EncryptedReadStatus::Truncated) && ok;

    ok = expect(
             security::readEncryptedFile(canonicalPath, *key, context, plaintext.size() - 1).status
                 == EncryptedReadStatus::Oversized,
             "oversized plaintext was not rejected before decryption"
         )
        && ok;
    ok = expect(
             security::readEncryptedFile(root.path() / "absent.enc", *key, context, 64).status
                 == EncryptedReadStatus::NotFound,
             "absent file did not return not-found"
         )
        && ok;
    return ok;
  }

  bool swappedFilesFail() {
    TestRoot root;
    auto key = makeKey(47);
    if (!expect(key.has_value(), "failed to construct swap test key")) {
      return false;
    }
    const std::array<std::uint8_t, 1> first = {1};
    const std::array<std::uint8_t, 1> second = {2};
    const auto firstPath = root.path() / "first.enc";
    const auto secondPath = root.path() / "second.enc";
    const security::EncryptionContext firstContext{.purpose = "payload-v1", .objectId = "first"};
    const security::EncryptionContext secondContext{.purpose = "payload-v1", .objectId = "second"};

    bool ok = true;
    ok = expect(security::writeEncryptedFile(firstPath, first, *key, firstContext), "first swap write failed") && ok;
    ok = expect(security::writeEncryptedFile(secondPath, second, *key, secondContext), "second swap write failed")
        && ok;
    std::filesystem::copy_file(firstPath, secondPath, std::filesystem::copy_options::overwrite_existing);
    ok = expect(
             security::readEncryptedFile(secondPath, *key, secondContext, 64).status
                 == EncryptedReadStatus::AuthenticationFailed,
             "swapped file was accepted"
         )
        && ok;
    return ok;
  }

  bool privateAtomicWrites() {
    TestRoot root;
    auto key = makeKey(57);
    if (!expect(key.has_value(), "failed to construct permissions test key")) {
      return false;
    }
    const auto directory = root.path() / "private/store";
    const auto path = directory / "payload.enc";
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "permissions"};
    const std::array<std::uint8_t, 3> first = {1, 2, 3};
    const std::array<std::uint8_t, 4> replacement = {4, 5, 6, 7};

    bool ok = true;
    {
      UmaskGuard permissive(0000);
      ok = expect(security::writeEncryptedFile(path, first, *key, context), "private write failed") && ok;
      ok = expect(security::writeEncryptedFile(path, replacement, *key, context), "atomic replacement failed") && ok;
    }
    ok = expect(mode(directory) == FileUtils::privateDirectoryMode(), "encrypted directory mode was not 0700") && ok;
    ok = expect(mode(path) == FileUtils::privateFileMode(), "encrypted file mode was not 0600") && ok;
    ok = expect(!std::filesystem::exists(path.string() + ".tmp"), "atomic temporary file remained") && ok;
    const auto result = security::readEncryptedFile(path, *key, context, 64);
    ok = expect(result.succeeded() && result.plaintext == std::vector<std::uint8_t>(replacement.begin(), replacement.end()),
                "atomic replacement did not publish the new plaintext")
        && ok;
    return ok;
  }
} // namespace

int main() {
  if (!expect(security::initializeSecurityPrimitives(), "libsodium initialization failed")) {
    return 1;
  }

  bool ok = true;
  ok = secureBufferBehavior() && ok;
  ok = roundTrips() && ok;
  ok = noncesAndPlaintextHiding() && ok;
  ok = authenticationAndContextFailures() && ok;
  ok = parserFailures() && ok;
  ok = swappedFilesFail() && ok;
  ok = privateAtomicWrites() && ok;
  return ok ? 0 : 1;
}
