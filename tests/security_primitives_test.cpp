#include "security/encrypted_file_store.h"
#include "security/secure_buffer.h"
#include "tests/test_check.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace {
  using security::EncryptedReadStatus;

  std::vector<std::filesystem::path> g_testRoots;

  void removeTestRoots() {
    for (const auto& path : g_testRoots) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }

  class TestRoot {
  public:
    TestRoot() {
      const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
      m_path = std::filesystem::temp_directory_path() / ("noctalia-security-test-" + std::to_string(serial));
      std::filesystem::create_directories(m_path);
      if (g_testRoots.empty()) {
        std::atexit(removeTestRoots);
      }
      g_testRoots.push_back(m_path);
    }

    ~TestRoot() { std::filesystem::remove_all(m_path); }

    TestRoot(const TestRoot&) = delete;
    TestRoot& operator=(const TestRoot&) = delete;

    const std::filesystem::path& path() const { return m_path; }

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

  void secureBufferBehavior() {
    const std::array<std::uint8_t, 5> source = {1, 2, 0, 4, 5};
    security::SecureBuffer original(source);
    security::SecureBuffer moved(std::move(original));

    // Asserting the moved-from buffer is wiped is the point of this check.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    TEST_CHECK(original.empty());
    TEST_CHECK(std::ranges::equal(moved.bytes(), source));
    moved.clear();
    TEST_CHECK(moved.empty());

    security::SecureBuffer wrongSize(security::SecureKey::Size - 1);
    TEST_CHECK(!security::SecureKey::fromBuffer(std::move(wrongSize)).has_value());
    TEST_CHECK(security::SecureKey::generate().has_value());
  }

  void roundTrips() {
    TestRoot root;
    auto key = makeKey(7);
    TEST_CHECK(key.has_value());
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "round-trip"};

    std::vector<std::vector<std::uint8_t>> fixtures;
    fixtures.emplace_back();
    fixtures.emplace_back(std::initializer_list<std::uint8_t>{'h', 'e', 'l', 'l', 'o'});
    fixtures.emplace_back(std::initializer_list<std::uint8_t>{0, 1, 2, 0, 255});
    fixtures.emplace_back(10U * 1024U * 1024U, 0x5a);

    for (std::size_t index = 0; index < fixtures.size(); ++index) {
      const auto path = root.path() / (std::to_string(index) + ".enc");
      TEST_CHECK(security::writeEncryptedFile(path, fixtures[index], *key, context));
      const auto result = security::readEncryptedFile(path, *key, context, fixtures[index].size());
      TEST_CHECK(result.succeeded());
      TEST_CHECK(result.plaintext == fixtures[index]);
    }
  }

  void noncesAndPlaintextHiding() {
    TestRoot root;
    auto key = makeKey(17);
    TEST_CHECK(key.has_value());
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "nonce"};
    const std::string secret = "distinct plaintext marker 90210";
    const auto plaintext = std::as_bytes(std::span(secret));
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
    const auto firstPath = root.path() / "first.enc";
    const auto secondPath = root.path() / "second.enc";

    TEST_CHECK(security::writeEncryptedFile(firstPath, bytes, *key, context));
    TEST_CHECK(security::writeEncryptedFile(secondPath, bytes, *key, context));
    const auto first = readBytes(firstPath);
    const auto second = readBytes(secondPath);
    TEST_CHECK(first != second);
    const auto firstText = std::string_view(reinterpret_cast<const char*>(first.data()), first.size());
    TEST_CHECK(!firstText.contains(secret));
  }

  void authenticationAndContextFailures() {
    TestRoot root;
    auto key = makeKey(27);
    auto wrongKey = makeKey(28);
    TEST_CHECK(key.has_value() && wrongKey.has_value());
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "alpha"};
    const std::array<std::uint8_t, 4> plaintext = {4, 3, 2, 1};
    const auto path = root.path() / "auth.enc";

    TEST_CHECK(security::writeEncryptedFile(path, plaintext, *key, context));
    TEST_CHECK(
        security::readEncryptedFile(path, *wrongKey, context, 64).status == EncryptedReadStatus::AuthenticationFailed
    );
    TEST_CHECK(
        security::readEncryptedFile(path, *key, {.purpose = "other-purpose-v1", .objectId = "alpha"}, 64).status
        == EncryptedReadStatus::AuthenticationFailed
    );
    TEST_CHECK(
        security::readEncryptedFile(path, *key, {.purpose = "test-data-v1", .objectId = "beta"}, 64).status
        == EncryptedReadStatus::AuthenticationFailed
    );

    auto envelope = readBytes(path);
    envelope.back() ^= 0x80;
    TEST_CHECK(writeBytes(path, envelope));
    const auto modified = security::readEncryptedFile(path, *key, context, 64);
    TEST_CHECK(modified.status == EncryptedReadStatus::AuthenticationFailed);
    TEST_CHECK(modified.plaintext.empty());
  }

  void parserFailures() {
    TestRoot root;
    auto key = makeKey(37);
    TEST_CHECK(key.has_value());
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "parser"};
    const std::array<std::uint8_t, 6> plaintext = {1, 2, 3, 4, 5, 6};
    const auto canonicalPath = root.path() / "canonical.enc";
    TEST_CHECK(security::writeEncryptedFile(canonicalPath, plaintext, *key, context));
    const auto canonical = readBytes(canonicalPath);

    const auto checkMutation = [&](std::string_view name, std::vector<std::uint8_t> bytes,
                                   EncryptedReadStatus expected) {
      const auto path = root.path() / (std::string(name) + ".enc");
      TEST_CHECK(writeBytes(path, bytes));
      TEST_CHECK(security::readEncryptedFile(path, *key, context, 64).status == expected);
    };

    auto invalidMagic = canonical;
    invalidMagic[0] ^= 1;
    checkMutation("invalid magic", std::move(invalidMagic), EncryptedReadStatus::InvalidHeader);

    auto unsupportedVersion = canonical;
    unsupportedVersion[security::EncryptedFileMagic.size()] = 2;
    checkMutation("unsupported version", std::move(unsupportedVersion), EncryptedReadStatus::UnsupportedVersion);

    auto unsupportedAlgorithm = canonical;
    unsupportedAlgorithm[security::EncryptedFileMagic.size() + 1] = 2;
    checkMutation("unsupported algorithm", std::move(unsupportedAlgorithm), EncryptedReadStatus::UnsupportedAlgorithm);

    auto modifiedNonce = canonical;
    modifiedNonce[security::EncryptedFileMagic.size() + 2] ^= 1;
    checkMutation("modified nonce", std::move(modifiedNonce), EncryptedReadStatus::AuthenticationFailed);

    auto appended = canonical;
    appended.push_back(0);
    checkMutation("appended garbage", std::move(appended), EncryptedReadStatus::AuthenticationFailed);

    auto truncatedHeader = canonical;
    truncatedHeader.resize(security::EncryptedFileHeaderSize - 1);
    checkMutation("truncated header", std::move(truncatedHeader), EncryptedReadStatus::Truncated);

    auto truncatedTag = canonical;
    truncatedTag.resize(security::EncryptedFileHeaderSize + security::EncryptedFileTagSize - 1);
    checkMutation("truncated tag", std::move(truncatedTag), EncryptedReadStatus::Truncated);

    TEST_CHECK(
        security::readEncryptedFile(canonicalPath, *key, context, plaintext.size() - 1).status
        == EncryptedReadStatus::Oversized
    );
    TEST_CHECK(
        security::readEncryptedFile(root.path() / "absent.enc", *key, context, 64).status
        == EncryptedReadStatus::NotFound
    );
  }

  void swappedFilesFail() {
    TestRoot root;
    auto key = makeKey(47);
    TEST_CHECK(key.has_value());
    const std::array<std::uint8_t, 1> first = {1};
    const std::array<std::uint8_t, 1> second = {2};
    const auto firstPath = root.path() / "first.enc";
    const auto secondPath = root.path() / "second.enc";
    const security::EncryptionContext firstContext{.purpose = "payload-v1", .objectId = "first"};
    const security::EncryptionContext secondContext{.purpose = "payload-v1", .objectId = "second"};

    TEST_CHECK(security::writeEncryptedFile(firstPath, first, *key, firstContext));
    TEST_CHECK(security::writeEncryptedFile(secondPath, second, *key, secondContext));
    std::filesystem::copy_file(firstPath, secondPath, std::filesystem::copy_options::overwrite_existing);
    TEST_CHECK(
        security::readEncryptedFile(secondPath, *key, secondContext, 64).status
        == EncryptedReadStatus::AuthenticationFailed
    );
  }

  void privateAtomicWrites() {
    TestRoot root;
    auto key = makeKey(57);
    TEST_CHECK(key.has_value());
    const auto directory = root.path() / "private/store";
    const auto path = directory / "payload.enc";
    const security::EncryptionContext context{.purpose = "test-data-v1", .objectId = "permissions"};
    const std::array<std::uint8_t, 3> first = {1, 2, 3};
    const std::array<std::uint8_t, 4> replacement = {4, 5, 6, 7};

    {
      UmaskGuard permissive(0000);
      TEST_CHECK(security::writeEncryptedFile(path, first, *key, context));
      TEST_CHECK(security::writeEncryptedFile(path, replacement, *key, context));
    }
    TEST_CHECK(mode(directory) == FileUtils::privateDirectoryMode());
    TEST_CHECK(mode(path) == FileUtils::privateFileMode());
    TEST_CHECK(!std::filesystem::exists(path.string() + ".tmp"));
    const auto result = security::readEncryptedFile(path, *key, context, 64);
    TEST_CHECK(
        result.succeeded() && result.plaintext == std::vector<std::uint8_t>(replacement.begin(), replacement.end())
    );
  }
} // namespace

int main() {
  TEST_CHECK(security::initializeSecurityPrimitives());

  secureBufferBehavior();
  roundTrips();
  noncesAndPlaintextHiding();
  authenticationAndContextFailures();
  parserFailures();
  swappedFilesFail();
  privateAtomicWrites();
  return 0;
}
