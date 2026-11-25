#pragma once

#include "core/timer_manager.h"
#include "security/security_hardware_parsers.h"
#include "system/process.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The security key plugged in (e.g. a YubiKey) and what it can do, for Settings > Security:
//
// - FIDO2/U2F for pam_u2f: registers the key where sudo and polkit read keys (pam_u2f's per-user
//   default, $XDG_CONFIG_HOME/Yubico/u2f_keys, read from their PAM lines), removes registrations
//   by key handle, tests the key against the lock screen's PAM service.
// - The OpenPGP card: imports or updates its public key (the card's URL, gpg's keyservers or a
//   file), links it to the card, trusts it, takes it out of the keyring again.
// - Resident SSH keys: imports them into ~/.ssh (ssh-keygen -K), makes new ones on the key, removes
//   a copy, deletes a key from the security key, and writes ~/.ssh/config.d/<host>.conf for the git
//   host a key names.
//
// One operation runs at a time, off the main thread; results are applied on the main loop. An
// operation that needs the key's PIN sets pinPrompt() and waits for submitPin(); a wrong PIN sets
// it again with the error. The PIN never goes on a command line or into the environment: it goes
// to pamu2fcfg and fido2-token on stdin, and to ssh-keygen through an SSH_ASKPASS script that reads
// it once from a 0600 file in a private 0700 folder under $XDG_RUNTIME_DIR.
//
// All state lives here so the settings page, rebuilt on every change, can stay stateless.
class SecurityKeyManager {
public:
  using ChangedCallback = std::function<void()>;
  using Device = security_hw::FidoDevice;
  using SshCreateOptions = security_hw::SshCreateOptions;

  enum class Area : std::uint8_t { Fido, Gpg, Ssh };
  enum class Op : std::uint8_t {
    None,
    FidoRegister,
    FidoRemove,
    FidoPin,
    GpgImport,
    GpgImportFile,
    GpgTrust,
    GpgRemove,
    SshImport,
    SshCreate,
    SshRemove,
    SshDelete,
    SshHost,
    SshPersist,
  };

  struct Status {
    std::string message;
    bool error = false;
  };

  struct Credential {
    std::string handle;
    std::string label;
    std::string raw;
    bool unused = false; // sudo and polkit don't read the user's own list
  };

  struct CardInfo {
    std::string reader;
    std::string vendor;
    std::string serial;
    std::string holder;
    std::string url;
    std::vector<std::string> fingerprints; // signature, encryption, authentication (non-empty only)
  };
  enum class KeyState : std::uint8_t { None, Missing, PublicOnly, Ready };

  enum class HostState : std::uint8_t { None, Off, On, Edited, Foreign };
  struct SshKey {
    std::string name; // in ~/.ssh
    std::string application;
    std::string type;
    std::string user;
    std::string fingerprint;
    std::string publicKey;
    bool encrypted = false;
    std::string host; // the git host the application names, or ""
    HostState hostState = HostState::None;
    int persistMinutes = 0;
  };
  struct SshCreated {
    std::string name;
    std::string publicKey;
    std::string host;
  };

  struct PinPrompt {
    Op op = Op::None;
    std::string error;
  };

  SecurityKeyManager();
  ~SecurityKeyManager();

  SecurityKeyManager(const SecurityKeyManager&) = delete;
  SecurityKeyManager& operator=(const SecurityKeyManager&) = delete;

  void setChangedCallback(ChangedCallback callback);

  // The tools each block runs, looked up on PATH at construction and on every refresh():
  // pamu2fcfg registers FIDO2 keys, fido2-token lists them, gpg reads the card, ssh-keygen makes
  // resident SSH keys.
  bool fido2ToolsAvailable() const noexcept { return m_tools.pamu2fcfg && m_tools.fido2Token; }
  bool gpgAvailable() const noexcept { return m_tools.gpg; }
  bool sshAvailable() const noexcept { return m_tools.sshKeygen && m_tools.fido2Token; }
  static std::filesystem::path authfilePath();

  // Re-reads the keys plugged in, the registrations, the OpenPGP card and ~/.ssh.
  void refresh();
  bool loaded() const noexcept { return m_loaded; }
  // Hot plug: while started, polls the keys plugged in and refreshes when they change.
  void startWatching();
  void stopWatching();

  // The operation running, if any, and what it waits for ("touch", "fetch", …).
  Op op() const noexcept { return m_op; }
  bool busy() const noexcept { return m_op != Op::None; }
  const std::string& stage() const noexcept { return m_stage; }
  // Stops the running operation (killing its process); nothing it started is left half done.
  void cancel();
  const Status& status(Area area) const;
  void setStatus(Area area, std::string message, bool error);
  void clearStatus(Area area);

  const std::optional<PinPrompt>& pinPrompt() const noexcept { return m_pinPrompt; }
  void submitPin(std::string pin);
  void cancelPin();

  // -- FIDO2/U2F ------------------------------------------------------------------------------
  const std::vector<Device>& devices() const noexcept { return m_devices; }
  const Device* device() const noexcept { return m_devices.empty() ? nullptr : &m_devices.front(); }
  const std::vector<Credential>& credentials() const noexcept { return m_credentials; }
  const security_hw::FidoTarget& target() const noexcept { return m_target; }
  // The user's keys in the file sudo and polkit read, when that isn't their own (-1: unreadable).
  int otherCount() const noexcept { return m_otherCount; }
  // sudo or polkit only take a key with its PIN.
  bool pinRequired() const noexcept { return m_pinRequired; }
  // Every registered key asks for its PIN before the touch (+pin on the user's credentials).
  bool pinForSignIn() const noexcept { return m_pinForSignIn; }
  // sudo or polkit never ask for the PIN (pinverification=0): +pin has no effect there.
  bool pinForSignInOverridden() const noexcept { return m_pinOff; }
  // Adds or takes out +pin on every registered key; later registrations follow it.
  void setPinForSignIn(bool required);
  bool canRegister() const;
  void requestRegister();
  void removeCredential(const std::string& handle);

  // -- OpenPGP card ---------------------------------------------------------------------------
  const std::optional<CardInfo>& card() const noexcept { return m_card; }
  KeyState keyState() const noexcept { return m_keyState; }
  bool ultimatelyTrusted() const noexcept { return m_trusted; }
  // Imports (or updates) the card's public key from the card URL, then gpg's keyservers.
  void importPublicKey(bool offline);
  void importPublicKeyFromFile(const std::filesystem::path& file);
  void trustKey();
  // Takes the card's key out of the keyring; never a secret part kept on disk.
  void removeKey();

  // -- Resident SSH keys ----------------------------------------------------------------------
  const std::vector<SshKey>& sshKeys() const noexcept { return m_sshKeys; }
  // ~/.ssh/config is a symbolic link without the Include of config.d.
  bool sshConfigIsForeignLink() const noexcept { return m_sshIncludeLink; }
  // A random user ID and user@host, for a new key.
  SshCreateOptions sshDefaults() const;
  const std::optional<SshCreated>& sshCreated() const noexcept { return m_sshCreated; }
  void clearSshCreated();
  // Set when the security key already holds a key for that application and user ID.
  const std::optional<SshCreateOptions>& sshReplaceCandidate() const noexcept { return m_sshReplace; }
  void clearSshReplaceCandidate();
  void requestSshImport();
  // "" when the options can be used, else the problem (see security_hw::sshCreateProblem).
  static std::string sshCreateProblem(const SshCreateOptions& options);
  void requestSshCreate(SshCreateOptions options);
  void removeSshCopy(const std::string& name);
  void requestSshDelete(const std::string& name);
  void setSshHost(const std::string& name, bool enable);
  void setSshPersist(const std::string& name, int minutes);

private:
  using ResultHandler = std::function<void(const process::RunResult&)>;
  struct RunExtras {
    std::optional<std::string> stdinData;
    int timeoutSeconds = 20;
    std::vector<process::EnvOverride> env;
    std::string workingDir;
  };

  // Runs args on a worker; `done` runs on the main loop unless this manager is gone.
  bool run(const std::vector<std::string>& args, ResultHandler done, RunExtras extras);
  bool run(const std::vector<std::string>& args, ResultHandler done) { return run(args, std::move(done), {}); }

  bool beginOp(Op op, Area area);
  void finishOp();
  bool cancelled() const;
  void succeed(Area area, std::string message);
  void fail(Area area, const std::string& code, const std::string& detail = {});
  // A PIN error of an operation that takes one opens the prompt again; returns whether it did.
  bool reaskPin(Op op, const std::string& code);
  void startWithPin(Op op, const std::string& pin);
  Status& statusRef(Area area);
  void setStage(std::string stage);

  void detectTools();
  // Everything refresh() reads except the keys plugged in, which the caller loads.
  void refreshAfterDevices();
  void loadDevices(const std::function<void()>& then = {});
  void loadDeviceInfo(
      std::shared_ptr<std::vector<Device>> found, std::size_t index, std::uint64_t generation,
      std::function<void()> then
  );
  void probe();
  void loadFidoState();
  std::vector<std::string> readOwnCredentials(std::vector<std::string>* others) const;
  bool writeCredentials(const std::vector<std::string>& mine, const std::vector<std::string>& others);
  void checkAlreadyRegistered(
      std::shared_ptr<std::vector<std::string>> handles, std::size_t index, std::function<void(bool)> then
  );
  void runRegister(const std::string& pin);

  void loadCard(const std::function<void()>& then = {});
  void loadKeyState(const std::function<void()>& then = {});
  void fetchFromKeyserver(bool updating);
  void linkCard(std::string message);

  void loadSsh();
  std::filesystem::path sshDir() const;
  std::optional<std::string> readSsh(const std::filesystem::path& relative, bool follow) const;
  std::optional<std::string> sshConfigText() const;
  std::optional<security_hw::SkPublic> sshPublic(const std::string& name) const;
  security_hw::DropIn dropIn(const std::string& host) const;
  struct HostChange {
    std::filesystem::path path;
    std::optional<std::string> text; // nullopt deletes the drop-in
  };
  struct HostPlan {
    std::string code; // non-empty: the drop-in isn't ours to change
    std::string detail;
    std::optional<HostChange> change;
  };
  HostPlan planHost(
      const std::string& name, const security_hw::GitRemote& remote, bool enable, std::optional<int> minutes
  ) const;
  HostPlan hostChangeFor(const std::string& name, bool enable) const;
  // Returns an error code ("" on success) with its detail.
  std::string_view applyHost(const std::optional<HostChange>& change, std::string& detail);
  std::string_view ensureInclude(std::string& detail);
  void unlinkPair(const std::string& name);
  // Copies complete pairs from <workdir> into ~/.ssh, never over a file.
  void installPairs(
      const std::filesystem::path& workdir, const std::vector<std::string>& names, std::vector<std::string>& imported,
      std::vector<std::string>& skipped
  );
  // A private folder for ssh-keygen with the askpass script and, when given, the PIN.
  std::optional<std::filesystem::path> makeWorkdir(const std::string& pin);
  std::vector<process::EnvOverride> askpassEnv() const;
  void runSshImport(const std::string& pin);
  void runSshCreate(const std::string& pin);
  void runSshDelete(const std::string& pin);

  void notifyChanged();

  std::shared_ptr<void> m_alive = std::make_shared<int>(0);
  ChangedCallback m_onChanged;
  std::string m_user;
  bool m_loaded = false;

  Op m_op = Op::None;
  Area m_opArea = Area::Fido;
  std::string m_stage;
  std::shared_ptr<std::atomic<bool>> m_opCancel;
  std::optional<std::filesystem::path> m_workdir;
  Status m_fidoStatus;
  Status m_gpgStatus;
  Status m_sshStatus;
  std::optional<PinPrompt> m_pinPrompt;

  struct Tools {
    bool pamu2fcfg = false;
    bool fido2Token = false;
    bool fido2Assert = false;
    bool gpg = false;
    bool sshKeygen = false;
  };
  Tools m_tools;

  Timer m_probeTimer;
  bool m_probing = false;
  // The HID device nodes at the last probe: fido2-token only runs when they change.
  std::vector<std::string> m_hidNodes;
  std::uint64_t m_devicesGeneration = 0;
  std::vector<Device> m_devices;

  std::vector<Credential> m_credentials;
  security_hw::FidoTarget m_target;
  int m_otherCount = 0;
  bool m_pinRequired = false;
  bool m_pinForSignIn = false;
  bool m_pinOff = false;

  std::optional<CardInfo> m_card;
  bool m_cardLoading = false;
  std::string m_primaryFingerprint;
  KeyState m_keyState = KeyState::None;
  bool m_trusted = false;

  std::vector<SshKey> m_sshKeys;
  bool m_sshIncludeLink = false;
  std::optional<SshCreated> m_sshCreated;
  std::optional<SshCreateOptions> m_sshReplace;
  std::optional<SshCreateOptions> m_pendingCreate;
  std::string m_pendingName;
};
