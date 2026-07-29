#include "security/secret_store.h"
#include "security/secure_buffer.h"
#include "security/storage_key_provider.h"
#include "wayland/clipboard_service.h"

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
#include <print>
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

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "clipboard_storage_permissions_test: {}", message);
    }
    return condition;
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

    [[nodiscard]] std::size_t storeCount() const {
      std::scoped_lock lock(m_mutex);
      return m_storeCount;
    }

    [[nodiscard]] std::size_t lookupCount() const {
      std::scoped_lock lock(m_mutex);
      return m_lookupCount;
    }

    [[nodiscard]] std::size_t eraseCount() const {
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
      m_keys.setChangeCallback([this]() { m_clipboard.syncPersistence(); });
    }

    void configure(StorageKeySource source, std::string keyFile = {}) {
      m_keys.configure(source, std::move(keyFile), m_clipboard.hasEncryptedPersistence());
    }

    [[nodiscard]] ClipboardService& clipboard() { return m_clipboard; }
    [[nodiscard]] security::StorageKeyProvider& keys() { return m_keys; }

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

  bool dispatchCompletion(security::SecretStore& store) {
    std::vector<pollfd> fds;
    const std::size_t start = store.addPollFds(fds);
    if (!expect(start == 0 && fds.size() == 1, "SecretStore did not publish one poll fd")) {
      return false;
    }
    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 2000);
    if (!expect(ready == 1, "timed out waiting for SecretStore completion")) {
      return false;
    }
    store.dispatch(fds, start);
    return true;
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

  bool migrationAndEncryptedReload(const std::filesystem::path& stateHome) {
    namespace fs = std::filesystem;
    const fs::path clipboardDir = stateHome / "noctalia/clipboard";
    const fs::path entriesDir = clipboardDir / "entries";
    const fs::path encryptedManifest = clipboardDir / "index.enc";
    const fs::path encryptedPayload = entriesDir / "entry.enc";

    seedLegacyHistory(stateHome);
    auto backend = std::make_unique<FakeSecretStoreBackend>();
    auto* fake = backend.get();
    security::SecretStore store(std::move(backend));
    bool ok = true;

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::SecretService);
      ok = dispatchCompletion(store) && ok;
      ok = dispatchCompletion(store) && ok;
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::Ready, "migration did not reach ready state"
           )
          && ok;
      ok = expect(!clipboard.persistenceMigrationPending(), "migration remained pending") && ok;
      ok = expect(clipboard.history().size() == 1, "legacy history was not loaded") && ok;
      ok = expect(clipboard.ensureEntryLoaded(0), "migrated payload was not available") && ok;
      ok = expect(clipboard.clipboardText() == "secret", "migrated plaintext changed") && ok;
      ok = expect(!fs::exists(clipboardDir / "index.json"), "legacy manifest was not removed") && ok;
      ok = expect(!fs::exists(entriesDir / "entry.bin"), "legacy payload was not removed") && ok;
      ok = expect(fs::exists(encryptedManifest), "encrypted manifest was not written") && ok;
      ok = expect(fs::exists(encryptedPayload), "encrypted payload was not written") && ok;
      ok = expect(mode(clipboardDir) == privateDirectoryMode(), "clipboard directory mode was not 0700") && ok;
      ok = expect(mode(entriesDir) == privateDirectoryMode(), "entries directory mode was not 0700") && ok;
      ok = expect(mode(encryptedManifest) == privateFileMode(), "encrypted manifest mode was not 0600") && ok;
      ok = expect(mode(encryptedPayload) == privateFileMode(), "encrypted payload mode was not 0600") && ok;
      ok = expect(!readFile(encryptedManifest).contains("secret"), "manifest leaked its text preview") && ok;
      ok = expect(!readFile(encryptedPayload).contains("secret"), "payload remained plaintext") && ok;
    }

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::SecretService);
      ok = dispatchCompletion(store) && ok;
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::Ready,
               "encrypted reload did not reach ready state"
           )
          && ok;
      ok = expect(clipboard.history().size() == 1, "encrypted history was not loaded") && ok;
      ok = expect(clipboard.ensureEntryLoaded(0), "encrypted payload could not be decrypted") && ok;
      ok = expect(clipboard.clipboardText() == "secret", "decrypted clipboard text changed") && ok;
    }

    ok = expect(fake->storeCount() == 1, "clipboard data key was unexpectedly replaced") && ok;
    return ok;
  }

  bool unavailableProviderPreservesPlaintext(const std::filesystem::path& stateHome) {
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

    bool ok = dispatchCompletion(store);
    ok = expect(
             clipboard.persistenceState() == ClipboardPersistenceState::Unavailable,
             "unavailable provider state was not exposed"
         )
        && ok;
    ok = expect(fake->storeCount() == 0, "a key was stored while the provider was unavailable") && ok;
    ok = expect(readFile(manifest) == manifestBefore, "legacy manifest changed without a keyring") && ok;
    ok = expect(readFile(payload) == payloadBefore, "legacy payload changed without a keyring") && ok;
    ok = expect(!fs::exists(stateHome / "noctalia/clipboard/index.enc"), "encrypted manifest was created") && ok;

    fake->setLookupStatus(std::nullopt);
    clipboard.retryPersistence();
    ok = dispatchCompletion(store) && ok;
    ok = dispatchCompletion(store) && ok;
    ok = dispatchCompletion(store) && ok;
    ok = expect(
             clipboard.persistenceState() == ClipboardPersistenceState::Ready,
             "retry did not migrate after the provider became available"
         )
        && ok;
    ok = expect(!fs::exists(manifest), "retry left the legacy manifest behind") && ok;
    ok = expect(!fs::exists(payload), "retry left the legacy payload behind") && ok;
    ok = expect(fs::exists(stateHome / "noctalia/clipboard/index.enc"), "retry did not create encrypted history") && ok;
    return ok;
  }

  bool missingKeyPreservesEncryptedFiles(const std::filesystem::path& stateHome) {
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

    bool ok = dispatchCompletion(store);
    ok = expect(
             clipboard.persistenceState() == ClipboardPersistenceState::MissingKey,
             "missing encrypted-history key was not exposed"
         )
        && ok;
    ok = expect(fake->storeCount() == 0, "missing encrypted-history key was silently replaced") && ok;
    ok = expect(readFile(manifest) == before, "encrypted history changed when its key was missing") && ok;
    return ok;
  }

  bool secretServiceRecoveryReset(const std::filesystem::path& stateHome) {
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

    bool ok = dispatchCompletion(store);
    ok = expect(
             clipboard.persistenceState() == ClipboardPersistenceState::MissingKey,
             "recovery fixture did not expose its missing key"
         )
        && ok;
    ok = expect(clipboard.clearEncryptedPersistenceForRecovery(), "failed to clear encrypted clipboard storage") && ok;
    ok = expect(!fs::exists(manifest), "recovery left the encrypted manifest behind") && ok;
    ok = expect(!fs::exists(encryptedPayload), "recovery left an encrypted payload behind") && ok;
    ok = expect(fs::exists(legacyPayload), "recovery removed a legacy plaintext payload") && ok;
    ok = expect(clipboard.history().empty(), "recovery retained clipboard history metadata") && ok;

    std::optional<bool> resetResult;
    harness.keys().resetAfterEncryptedDataCleared([&resetResult](bool success) { resetResult = success; });
    ok = dispatchCompletion(store) && ok;
    ok = dispatchCompletion(store) && ok;
    ok = expect(resetResult == true, "Secret Service recovery did not complete successfully") && ok;
    ok = expect(
             clipboard.persistenceState() == ClipboardPersistenceState::Ready,
             "Secret Service recovery did not reopen clipboard persistence"
         )
        && ok;
    ok = expect(fake->eraseCount() == 1, "Secret Service recovery did not erase the old master key") && ok;
    ok = expect(fake->storeCount() == 1, "Secret Service recovery did not store one replacement master key") && ok;
    return ok;
  }

  bool fileKeySource(const std::filesystem::path& stateHome) {
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
    bool ok = true;

    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::Ready,
               "file key source did not reach ready state"
           )
          && ok;
      ok = expect(clipboard.clipboardText() == "secret", "file key source did not migrate legacy history") && ok;
      ok = expect(fs::exists(encryptedManifest), "file key source did not create encrypted history") && ok;
      ok = expect(
               !fs::exists(stateHome / "noctalia/clipboard/index.json"),
               "file key source left the legacy manifest behind"
           )
          && ok;
      auto clipboardKey = harness.keys().deriveKey(security::StorageKeyPurpose::ClipboardHistory);
      auto calendarKey = harness.keys().deriveKey(security::StorageKeyPurpose::CalendarEvents);
      ok = expect(
               clipboardKey.has_value()
                   && calendarKey.has_value()
                   && !std::ranges::equal(clipboardKey->bytes(), calendarKey->bytes()),
               "storage master key did not derive distinct purpose keys"
           )
          && ok;
    }

    const std::string encryptedBefore = readFile(encryptedManifest);
    ok = expect(fake->lookupCount() == 0, "file key source queried Secret Service") && ok;
    ok = expect(fake->storeCount() == 0, "file key source stored a Secret Service key") && ok;

    {
      std::ofstream output(keyFile, std::ios::trunc);
      output << keyB << '\n';
    }
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::RecoveryRequired,
               "changed file key did not fail authentication"
           )
          && ok;
      ok = expect(readFile(encryptedManifest) == encryptedBefore, "changed file key overwrote encrypted history") && ok;
    }

    {
      std::ofstream output(keyFile, std::ios::trunc);
      output << std::string(64, 'A');
    }
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      ok =
          expect(
              clipboard.persistenceState() == ClipboardPersistenceState::BackendError, "malformed file key was accepted"
          )
          && ok;
      ok = expect(readFile(encryptedManifest) == encryptedBefore, "malformed file key changed encrypted history") && ok;
    }

    fs::remove(keyFile);
    {
      ClipboardStorageHarness harness(store);
      ClipboardService& clipboard = harness.clipboard();
      harness.configure(StorageKeySource::File, keyFile.string());
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::MissingKey,
               "missing configured key file was not exposed"
           )
          && ok;
      ok = expect(readFile(encryptedManifest) == encryptedBefore, "missing file key changed encrypted history") && ok;

      {
        std::ofstream output(keyFile, std::ios::trunc);
        output << keyA << '\n';
      }
      clipboard.retryPersistence();
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::Ready,
               "retry did not reopen history after the file key was provisioned"
           )
          && ok;
      ok = expect(clipboard.clipboardText() == "secret", "retried file key decrypted the wrong payload") && ok;

      const std::string keyFileBeforeReset = readFile(keyFile);
      ok = expect(
               clipboard.clearEncryptedPersistenceForRecovery(),
               "file-backed recovery failed to clear encrypted clipboard storage"
           )
          && ok;
      std::optional<bool> resetResult;
      harness.keys().resetAfterEncryptedDataCleared([&resetResult](bool success) { resetResult = success; });
      ok = expect(resetResult == true, "file-backed recovery did not reopen storage") && ok;
      ok = expect(
               clipboard.persistenceState() == ClipboardPersistenceState::Ready,
               "file-backed recovery did not restore ready state"
           )
          && ok;
      ok = expect(readFile(keyFile) == keyFileBeforeReset, "file-backed recovery modified the managed key file") && ok;
      ok = expect(!fs::exists(encryptedManifest), "file-backed recovery left encrypted history behind") && ok;
    }

    ok = expect(fake->lookupCount() == 0, "file key source fell back to Secret Service") && ok;
    ok = expect(fake->storeCount() == 0, "file key source mutated Secret Service") && ok;
    ok = expect(fake->eraseCount() == 0, "file key source erased a Secret Service key") && ok;
    return ok;
  }
} // namespace

int main() {
  namespace fs = std::filesystem;

  if (!expect(security::initializeSecurityPrimitives(), "failed to initialize libsodium")) {
    return 1;
  }

  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("noctalia-clipboard-storage-test-" + std::to_string(serial));
  fs::remove_all(root);
  fs::create_directories(root);

  bool ok = true;
  const fs::path migrationHome = root / "migration";
  ok = expect(::setenv("NOCTALIA_STATE_HOME", migrationHome.c_str(), 1) == 0, "failed to set migration state home")
      && ok;
  ok = migrationAndEncryptedReload(migrationHome) && ok;

  const fs::path unavailableHome = root / "unavailable";
  ok = expect(::setenv("NOCTALIA_STATE_HOME", unavailableHome.c_str(), 1) == 0, "failed to set unavailable state home")
      && ok;
  ok = unavailableProviderPreservesPlaintext(unavailableHome) && ok;

  const fs::path missingKeyHome = root / "missing-key";
  ok = expect(::setenv("NOCTALIA_STATE_HOME", missingKeyHome.c_str(), 1) == 0, "failed to set missing-key state home")
      && ok;
  ok = missingKeyPreservesEncryptedFiles(missingKeyHome) && ok;

  const fs::path recoveryHome = root / "recovery";
  ok = expect(::setenv("NOCTALIA_STATE_HOME", recoveryHome.c_str(), 1) == 0, "failed to set recovery state home") && ok;
  ok = secretServiceRecoveryReset(recoveryHome) && ok;

  const fs::path fileKeyHome = root / "file-key";
  ok = expect(::setenv("NOCTALIA_STATE_HOME", fileKeyHome.c_str(), 1) == 0, "failed to set file-key state home") && ok;
  ok = fileKeySource(fileKeyHome) && ok;

  ::unsetenv("NOCTALIA_STATE_HOME");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
