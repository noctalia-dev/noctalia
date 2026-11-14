#include "security/secret_store.h"
#include "security/secure_buffer.h"
#include "security/storage_key_provider.h"
#include "shell/wayland/clipboard_service.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using security::SecretStoreBackendResult;
  using security::SecretStoreErrorCategory;
  using security::SecretStoreStatus;

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

  // Paste reads the live selection, so persisted history is checked directly.
  std::optional<std::string> newestEntryText(ClipboardService& clipboard) {
    if (clipboard.history().empty() || !clipboard.ensureEntryLoaded(0)) {
      return std::nullopt;
    }
    const auto& data = clipboard.history().front().data;
    return std::string(data.begin(), data.end());
  }

  std::string keyFor(const security::SecretStoreAttributes& attributes) {
    return attributes.application
        + '\x1f'
        + attributes.scope
        + '\x1f'
        + attributes.owner
        + '\x1f'
        + attributes.name
        + '\x1f'
        + attributes.version;
  }

  class FakeSecretStoreBackend final : public security::SecretStoreBackend {
  public:
    SecretStoreBackendResult probe(security::SecretStoreCancellation&) override {
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
      };
    }

    SecretStoreBackendResult
    lookup(const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation&) override {
      std::scoped_lock lock(m_mutex);
      ++m_lookupCount;
      if (m_lookupStatus.has_value()) {
        return {
            .status = *m_lookupStatus,
            .errorCategory = *m_lookupStatus == SecretStoreStatus::Unavailable
                ? SecretStoreErrorCategory::ProviderUnavailable
                : SecretStoreErrorCategory::Other,
        };
      }
      const auto it = m_values.find(keyFor(attributes));
      if (it == m_values.end()) {
        return {
            .status = SecretStoreStatus::NotFound,
            .errorCategory = SecretStoreErrorCategory::None,
        };
      }
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
          .value = security::SecureBuffer(it->second),
      };
    }

    SecretStoreBackendResult store(
        const security::SecretStoreAttributes& attributes, std::span<const std::uint8_t> value, const std::string&,
        security::SecretStoreCancellation&
    ) override {
      std::scoped_lock lock(m_mutex);
      ++m_storeCount;
      m_values.insert_or_assign(keyFor(attributes), std::vector<std::uint8_t>(value.begin(), value.end()));
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
      };
    }

    SecretStoreBackendResult
    erase(const security::SecretStoreAttributes& attributes, security::SecretStoreCancellation&) override {
      std::scoped_lock lock(m_mutex);
      ++m_eraseCount;
      m_values.erase(keyFor(attributes));
      return {
          .status = SecretStoreStatus::Success,
          .errorCategory = SecretStoreErrorCategory::None,
      };
    }

    void setLookupStatus(std::optional<SecretStoreStatus> status) {
      std::scoped_lock lock(m_mutex);
      m_lookupStatus = status;
    }

    std::size_t storeCount() const {
      std::scoped_lock lock(m_mutex);
      return m_storeCount;
    }

    std::size_t lookupCount() const {
      std::scoped_lock lock(m_mutex);
      return m_lookupCount;
    }

    std::size_t eraseCount() const {
      std::scoped_lock lock(m_mutex);
      return m_eraseCount;
    }

  private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::vector<std::uint8_t>> m_values;
    std::optional<SecretStoreStatus> m_lookupStatus;
    std::size_t m_lookupCount = 0;
    std::size_t m_storeCount = 0;
    std::size_t m_eraseCount = 0;
  };

  class ClipboardStorageHarness {
  public:
    explicit ClipboardStorageHarness(security::SecretStore& store) : m_keys(store), m_clipboard(m_keys) {
      m_keys.setChangeCallback([this] { m_clipboard.syncPersistence(); });
    }

    void configure(StorageKeySource source, std::string keyFile = {}) {
      m_keys.configure(source, std::move(keyFile), m_clipboard.hasEncryptedPersistence());
    }

    ClipboardService& clipboard() { return m_clipboard; }
    security::StorageKeyProvider& keys() { return m_keys; }

  private:
    security::StorageKeyProvider m_keys;
    ClipboardService m_clipboard;
  };

  std::filesystem::perms mode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & permissionMask();
  }

  void makeWorldReadable(const std::filesystem::path& path, bool directory) {
    using P = std::filesystem::perms;
    const P requested = directory ? P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec
                                  : privateFileMode() | P::group_read | P::others_read;
    std::filesystem::permissions(path, requested, std::filesystem::perm_options::replace);
  }

  void dispatchCompletion(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    TEST_CHECK(start == 0 && fds.size() == 1);
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000);
    // timed out waiting for SecretStore completion
    TEST_CHECK(ready == 1);
    store.dispatch(fds, start);
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void seedLegacyHistory(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    const fs::path clipboardDir = stateHome / "noctalia/clipboard";
    const fs::path entriesDir = clipboardDir / "entries";
    const fs::path manifestPath = clipboardDir / "index.json";
    const fs::path payloadPath = entriesDir / "entry.bin";

    fs::create_directories(entriesDir);
    {
      std::ofstream payload(payloadPath, std::ios::binary | std::ios::trunc);
      payload << "secret";
    }
    {
      std::ofstream manifest(manifestPath, std::ios::trunc);
      manifest << nlohmann::json{
          {"entries",
           nlohmann::json::array({
               {
                   {"id", "entry"},
                   {"payload_path", "/tmp/ignored-noncanonical-path"},
                   {"mime_types", std::vector<std::string>{"text/plain"}},
                   {"data_mime_type", "text/plain"},
                   {"text_preview", "secret"},
                   {"byte_size", 6},
                   {"captured_at_ms", 0},
                   {"pinned", false},
               },
           })}
      }.dump(2);
    }
    makeWorldReadable(clipboardDir, true);
    makeWorldReadable(entriesDir, true);
    makeWorldReadable(manifestPath, false);
    makeWorldReadable(payloadPath, false);
  }

  void migrationAndEncryptedReload(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    const fs::path clipboardDir = stateHome / "noctalia/clipboard";
    const fs::path entriesDir = clipboardDir / "entries";
    const fs::path encryptedManifest = clipboardDir / "index.enc";
    const fs::path encryptedPayload = entriesDir / "entry.enc";

    seedLegacyHistory(stateHome);
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::SecretService);
      dispatchCompletion(store);
      dispatchCompletion(store);
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
      TEST_CHECK(!clipboard.persistenceMigrationPending());
      TEST_CHECK(clipboard.history().size() == 1);
      TEST_CHECK(clipboard.ensureEntryLoaded(0));
      TEST_CHECK(newestEntryText(clipboard) == "secret");
      TEST_CHECK(!fs::exists(clipboardDir / "index.json"));
      TEST_CHECK(!fs::exists(entriesDir / "entry.bin"));
      TEST_CHECK(fs::exists(encryptedManifest));
      TEST_CHECK(fs::exists(encryptedPayload));
      TEST_CHECK(mode(clipboardDir) == privateDirectoryMode());
      TEST_CHECK(mode(entriesDir) == privateDirectoryMode());
      TEST_CHECK(mode(encryptedManifest) == privateFileMode());
      TEST_CHECK(mode(encryptedPayload) == privateFileMode());
      // manifest leaked its text preview
      TEST_CHECK(!readFile(encryptedManifest).contains("secret"));
      // payload remained plaintext
      TEST_CHECK(!readFile(encryptedPayload).contains("secret"));
    }

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::SecretService);
      dispatchCompletion(store);
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
      TEST_CHECK(clipboard.history().size() == 1);
      TEST_CHECK(clipboard.ensureEntryLoaded(0));
      TEST_CHECK(newestEntryText(clipboard) == "secret");
    }

    // clipboard data key was unexpectedly replaced
    TEST_CHECK(fake->storeCount() == 1);
  }

  void unavailableProviderPreservesPlaintext(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    seedLegacyHistory(stateHome);
    const fs::path manifest = stateHome / "noctalia/clipboard/index.json";
    const fs::path payload = stateHome / "noctalia/clipboard/entries/entry.bin";
    const std::string manifestBefore = readFile(manifest);
    const std::string payloadBefore = readFile(payload);

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    fake->setLookupStatus(SecretStoreStatus::Unavailable);
    security::SecretStore store(std::move(backend));
    ClipboardStorageHarness harness(store);
    ClipboardService& clipboard = harness.clipboard();
    harness.configure(StorageKeySource::SecretService);

    dispatchCompletion(store);
    TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Unavailable);
    // a key was stored while the provider was unavailable
    TEST_CHECK(fake->storeCount() == 0);
    TEST_CHECK(readFile(manifest) == manifestBefore);
    TEST_CHECK(readFile(payload) == payloadBefore);
    TEST_CHECK(!fs::exists(stateHome / "noctalia/clipboard/index.enc"));

    fake->setLookupStatus(std::nullopt);
    clipboard.retryPersistence();
    dispatchCompletion(store);
    dispatchCompletion(store);
    dispatchCompletion(store);
    // retry did not migrate after the provider became available
    TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
    TEST_CHECK(!fs::exists(manifest));
    TEST_CHECK(!fs::exists(payload));
    TEST_CHECK(fs::exists(stateHome / "noctalia/clipboard/index.enc"));
  }

  void missingKeyPreservesEncryptedFiles(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    const fs::path clipboardDir = stateHome / "noctalia/clipboard";
    const fs::path manifest = clipboardDir / "index.enc";
    fs::create_directories(clipboardDir);
    {
      std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
      output << "existing-encrypted-envelope";
    }
    const std::string before = readFile(manifest);

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    ClipboardStorageHarness harness(store);
    ClipboardService& clipboard = harness.clipboard();
    harness.configure(StorageKeySource::SecretService);

    dispatchCompletion(store);
    TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::MissingKey);
    // missing encrypted-history key was silently replaced
    TEST_CHECK(fake->storeCount() == 0);
    TEST_CHECK(readFile(manifest) == before);
  }

  void secretServiceRecoveryReset(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    const fs::path clipboardDir = stateHome / "noctalia/clipboard";
    const fs::path entriesDir = clipboardDir / "entries";
    const fs::path manifest = clipboardDir / "index.enc";
    const fs::path encryptedPayload = entriesDir / "entry.enc";
    const fs::path legacyPayload = entriesDir / "legacy.bin";
    fs::create_directories(entriesDir);
    {
      std::ofstream output(manifest, std::ios::binary | std::ios::trunc);
      output << "unrecoverable-manifest";
    }
    {
      std::ofstream output(encryptedPayload, std::ios::binary | std::ios::trunc);
      output << "unrecoverable-payload";
    }
    {
      std::ofstream output(legacyPayload, std::ios::binary | std::ios::trunc);
      output << "legacy-payload";
    }

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    ClipboardStorageHarness harness(store);
    ClipboardService& clipboard = harness.clipboard();
    harness.configure(StorageKeySource::SecretService);

    dispatchCompletion(store);
    TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::MissingKey);
    TEST_CHECK(clipboard.clearEncryptedPersistenceForRecovery());
    TEST_CHECK(!fs::exists(manifest));
    TEST_CHECK(!fs::exists(encryptedPayload));
    // recovery removed a legacy plaintext payload
    TEST_CHECK(fs::exists(legacyPayload));
    // recovery retained clipboard history metadata
    TEST_CHECK(clipboard.history().empty());

    std::optional<bool> resetResult;
    harness.keys().resetAfterEncryptedDataCleared([&resetResult](bool success) { resetResult = success; });
    dispatchCompletion(store);
    dispatchCompletion(store);
    TEST_CHECK(resetResult == true);
    TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
    // Secret Service recovery did not erase the old master key
    TEST_CHECK(fake->eraseCount() == 1);
    // Secret Service recovery did not store one replacement master key
    TEST_CHECK(fake->storeCount() == 1);
  }

  void fileKeySource(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    constexpr std::string_view keyA = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    constexpr std::string_view keyB = "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100";

    seedLegacyHistory(stateHome);
    const fs::path keyFile = stateHome / "clipboard-key";
    {
      std::ofstream output(keyFile, std::ios::trunc);
      output << keyA << '\n';
    }

    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    const fs::path encryptedManifest = stateHome / "noctalia/clipboard/index.enc";

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
      // file key source did not migrate legacy history
      TEST_CHECK(newestEntryText(clipboard) == "secret");
      TEST_CHECK(fs::exists(encryptedManifest));
      TEST_CHECK(!fs::exists(stateHome / "noctalia/clipboard/index.json"));
      auto clipboardKey = harness.keys().deriveKey(security::StorageKeyPurpose::ClipboardHistory);
      auto calendarKey = harness.keys().deriveKey(security::StorageKeyPurpose::CalendarEvents);
      // storage master key did not derive distinct purpose keys
      TEST_CHECK(
          clipboardKey.has_value()
          && calendarKey.has_value()
          && !std::ranges::equal(clipboardKey->bytes(), calendarKey->bytes())
      );
    }

    const std::string encryptedBefore = readFile(encryptedManifest);
    // file key source queried Secret Service
    TEST_CHECK(fake->lookupCount() == 0);
    // file key source stored a Secret Service key
    TEST_CHECK(fake->storeCount() == 0);

    {
      std::ofstream output(keyFile, std::ios::trunc);
      output << keyB << '\n';
    }
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      // changed file key did not fail authentication
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::RecoveryRequired);
      TEST_CHECK(readFile(encryptedManifest) == encryptedBefore);
    }

    {
      std::ofstream output(keyFile, std::ios::trunc);
      output << std::string(64, 'A');
    }
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      // malformed file key was accepted
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::BackendError);
      TEST_CHECK(readFile(encryptedManifest) == encryptedBefore);
    }

    fs::remove(keyFile);
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::MissingKey);
      TEST_CHECK(readFile(encryptedManifest) == encryptedBefore);

      {
        std::ofstream output(keyFile, std::ios::trunc);
        output << keyA << '\n';
      }
      clipboard.retryPersistence();
      // retry did not reopen history after the file key was provisioned
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
      TEST_CHECK(newestEntryText(clipboard) == "secret");

      const std::string keyFileBeforeReset = readFile(keyFile);
      TEST_CHECK(clipboard.clearEncryptedPersistenceForRecovery());
      std::optional<bool> resetResult;
      harness.keys().resetAfterEncryptedDataCleared([&resetResult](bool success) { resetResult = success; });
      TEST_CHECK(resetResult == true);
      TEST_CHECK(clipboard.persistenceState() == ClipboardPersistenceState::Ready);
      // file-backed recovery modified the managed key file
      TEST_CHECK(readFile(keyFile) == keyFileBeforeReset);
      TEST_CHECK(!fs::exists(encryptedManifest));
    }

    // file key source fell back to Secret Service
    TEST_CHECK(fake->lookupCount() == 0);
    // file key source mutated Secret Service
    TEST_CHECK(fake->storeCount() == 0);
    // file key source erased a Secret Service key
    TEST_CHECK(fake->eraseCount() == 0);
  }
} // namespace

int main() {
  namespace fs = std::filesystem;

  TEST_CHECK(security::initializeSecurityPrimitives());

  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  static const fs::path root =
      fs::temp_directory_path() / ("noctalia-clipboard-storage-test-" + std::to_string(serial));
  fs::remove_all(root);
  fs::create_directories(root);
  // TEST_CHECK exits through std::exit, which runs this, so a failed check does not leave the
  // fixture tree behind.
  std::atexit([] {
    std::error_code ec;
    fs::remove_all(root, ec);
  });

  const fs::path migrationHome = root / "migration";
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", migrationHome.c_str(), 1) == 0);
  migrationAndEncryptedReload(migrationHome);

  const fs::path unavailableHome = root / "unavailable";
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", unavailableHome.c_str(), 1) == 0);
  unavailableProviderPreservesPlaintext(unavailableHome);

  const fs::path missingKeyHome = root / "missing-key";
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", missingKeyHome.c_str(), 1) == 0);
  missingKeyPreservesEncryptedFiles(missingKeyHome);

  const fs::path recoveryHome = root / "recovery";
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", recoveryHome.c_str(), 1) == 0);
  secretServiceRecoveryReset(recoveryHome);

  const fs::path fileKeyHome = root / "file-key";
  TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", fileKeyHome.c_str(), 1) == 0);
  fileKeySource(fileKeyHome);

  ::unsetenv("NOCTALIA_STATE_HOME");
  return 0;
}
