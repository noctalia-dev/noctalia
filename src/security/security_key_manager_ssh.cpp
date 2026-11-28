// Resident SSH keys: copies in ~/.ssh, new keys, deleting them, and the git hosts they name.

#include "core/log.h"
#include "i18n/i18n.h"
#include "security/private_files.h"
#include "security/security_key_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <sodium.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {
  constexpr Logger kLog("security-keys");

  constexpr int kTouchTimeoutSeconds = 60;
  // Making a resident SSH key waits for a touch, twice when it replaces one.
  constexpr int kCreateTimeoutSeconds = 120;
  constexpr int kAuthTimeoutSeconds = 120;
  constexpr std::size_t kSshFileLimit = 65536;

  using security_hw::lastLine;

  // ssh-keygen runs SSH_ASKPASS with the prompt as its only argument. This one answers a PIN
  // prompt once, from the file next to it, and removes the file; anything else gets nothing.
  constexpr std::string_view kAskpassScript = R"(#!/bin/sh
case "$1" in
  *PIN*) ;;
  *) exit 1 ;;
esac
pin="${0%/*}/pin"
[ -f "$pin" ] || exit 1
cat "$pin" && rm -f "$pin"
)";

  std::string sanitize(std::string_view text, std::string_view fallback) {
    std::string out;
    for (const char c : text) {
      if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-') {
        out.push_back(c);
      }
    }
    return out.empty() ? std::string(fallback) : out;
  }

  std::string runtimeDir() {
    if (const char* dir = std::getenv("XDG_RUNTIME_DIR"); dir != nullptr && *dir != '\0') {
      return dir;
    }
    return std::filesystem::temp_directory_path().string();
  }

  bool isDirectoryNotLink(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::symlink_status(path, ec));
  }

  // A private folder (0700); false when the path is a link or not a folder.
  bool ensurePrivateDir(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::symlink_status(path, ec))) {
      if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return false;
      }
    }
    return isDirectoryNotLink(path);
  }
} // namespace

std::filesystem::path SecurityKeyManager::sshDir() const {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / ".ssh";
  }
  if (const passwd* pw = ::getpwuid(::getuid()); pw != nullptr && pw->pw_dir != nullptr) {
    return std::filesystem::path(pw->pw_dir) / ".ssh";
  }
  return {};
}

std::optional<std::string> SecurityKeyManager::readSsh(const std::filesystem::path& relative, bool follow) const {
  // Never through a symbolic link unless <follow> (~/.ssh/config, which home-manager links to the
  // Nix store). Small ASCII files only.
  auto data = security_hw::files::readLimited(sshDir() / relative, follow, kSshFileLimit);
  if (data.has_value() && !std::ranges::all_of(*data, [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
    return std::nullopt;
  }
  return data;
}

std::optional<std::string> SecurityKeyManager::sshConfigText() const {
  std::error_code ec;
  if (!std::filesystem::exists(sshDir() / "config", ec)) {
    return std::string{};
  }
  return readSsh("config", true);
}

std::optional<security_hw::SkPublic> SecurityKeyManager::sshPublic(const std::string& name) const {
  const auto text = readSsh(name + ".pub", false);
  return text ? security_hw::parseSkPublic(*text) : std::nullopt;
}

security_hw::DropIn SecurityKeyManager::dropIn(const std::string& host) const {
  std::error_code ec;
  const auto relative = std::filesystem::path("config.d") / (host + ".conf");
  if (!std::filesystem::exists(std::filesystem::symlink_status(sshDir() / relative, ec))) {
    return {.kind = security_hw::DropInKind::Missing};
  }
  const auto text = readSsh(relative, false);
  if (!text) {
    return {.kind = security_hw::DropInKind::Foreign};
  }
  return security_hw::parseDropIn(*text);
}

void SecurityKeyManager::loadSsh() {
  m_sshKeys.clear();
  const auto dir = sshDir();
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (security_hw::isResidentKeyName(name)) {
      names.push_back(name);
    }
  }
  std::ranges::sort(names);

  const auto config = sshConfigText();
  const bool included = config.has_value() && security_hw::includesDropIns(*config);
  m_sshIncludeLink = !included && std::filesystem::is_symlink(dir / "config", ec);

  for (const auto& name : names) {
    SshKey key{.name = name};
    const auto pubText = readSsh(name + ".pub", false);
    if (const auto pub = pubText ? security_hw::parseSkPublic(*pubText) : std::nullopt) {
      key.application = pub->application;
      key.type = pub->type;
      key.user = security_hw::residentKeyUser(name, pub->type, pub->application);
      key.fingerprint = security_hw::sshFingerprint(pub->blob);
      key.publicKey = lastLine(*pubText);
      if (const auto remote = security_hw::gitRemote(pub->application)) {
        key.host = remote->host;
        const auto found = dropIn(remote->host);
        switch (found.kind) {
        case security_hw::DropInKind::Missing:
          key.hostState = HostState::Off;
          break;
        case security_hw::DropInKind::Edited:
          key.hostState = HostState::Edited;
          break;
        case security_hw::DropInKind::Foreign:
          key.hostState = HostState::Foreign;
          break;
        case security_hw::DropInKind::Ours:
          key.hostState = std::ranges::contains(found.names, name) && included ? HostState::On : HostState::Off;
          key.persistMinutes = found.minutes.value_or(0);
          break;
        }
      }
    }
    if (const auto text = readSsh(name, false)) {
      if (const auto handle = security_hw::parseSkHandle(*text)) {
        key.encrypted = handle->encrypted;
      }
    }
    m_sshKeys.push_back(std::move(key));
  }
}

SecurityKeyManager::SshCreateOptions SecurityKeyManager::sshDefaults() const {
  // A random user ID, so keys for the same application don't replace each other on the key.
  std::array<unsigned char, 8> random{};
  randombytes_buf(random.data(), random.size());
  std::array<char, 17> hex{};
  sodium_bin2hex(hex.data(), hex.size(), random.data(), random.size());
  std::array<char, 256> host{};
  if (::gethostname(host.data(), host.size() - 1) != 0) {
    host[0] = '\0';
  }
  return SshCreateOptions{
      .user = hex.data(),
      .comment = sanitize(m_user, "user") + "@" + sanitize(host.data(), "localhost"),
  };
}

std::string SecurityKeyManager::sshCreateProblem(const SshCreateOptions& options) {
  return security_hw::sshCreateProblem(options);
}

void SecurityKeyManager::clearSshCreated() {
  m_sshCreated.reset();
  notifyChanged();
}

void SecurityKeyManager::clearSshReplaceCandidate() {
  m_sshReplace.reset();
  clearStatus(Area::Ssh);
}

SecurityKeyManager::HostPlan SecurityKeyManager::planHost(
    const std::string& name, const security_hw::GitRemote& remote, bool enable, std::optional<int> minutes
) const {
  const auto found = dropIn(remote.host);
  const std::string shown = "~/.ssh/config.d/" + remote.host + ".conf";
  const auto path = sshDir() / "config.d" / (remote.host + ".conf");
  const int wanted = minutes.has_value() ? *minutes : found.minutes.value_or(0);
  const bool listed = std::ranges::contains(found.names, name);
  if (!enable) {
    if (found.kind == security_hw::DropInKind::Ours && listed) {
      std::vector<std::string> rest;
      for (const auto& n : found.names) {
        if (n != name) {
          rest.push_back(n);
        }
      }
      if (rest.empty()) {
        return {.change = HostChange{.path = path, .text = std::nullopt}};
      }
      return {.change = HostChange{.path = path, .text = security_hw::dropInText(rest, found.remote, wanted)}};
    }
    if (found.kind == security_hw::DropInKind::Edited && listed) {
      return {.code = "ssh-config-edited", .detail = shown};
    }
    return {};
  }
  if (found.kind == security_hw::DropInKind::Foreign) {
    return {.code = "ssh-config-foreign", .detail = shown};
  }
  if (found.kind == security_hw::DropInKind::Edited) {
    return {.code = "ssh-config-edited", .detail = shown};
  }
  if (found.kind == security_hw::DropInKind::Ours && !(found.remote == remote)) {
    // Another key names the same host with another login or port.
    return {.code = "ssh-config-conflict", .detail = remote.host};
  }
  std::vector<std::string> names = found.names;
  if (!listed) {
    names.push_back(name);
  }
  const std::string text = security_hw::dropInText(names, remote, wanted);
  if (found.kind == security_hw::DropInKind::Ours
      && listed
      && text == security_hw::dropInText(found.names, found.remote, found.minutes)) {
    return {};
  }
  return {.change = HostChange{.path = path, .text = text}};
}

SecurityKeyManager::HostPlan SecurityKeyManager::hostChangeFor(const std::string& name, bool enable) const {
  const auto pub = sshPublic(name);
  const auto remote = pub ? security_hw::gitRemote(pub->application) : std::nullopt;
  if (!remote) {
    return {};
  }
  return planHost(name, *remote, enable, std::nullopt);
}

std::string_view SecurityKeyManager::applyHost(const std::optional<HostChange>& change, std::string& detail) {
  if (!change.has_value()) {
    return {};
  }
  std::error_code ec;
  if (!change->text.has_value()) {
    std::filesystem::remove(change->path, ec);
    if (ec) {
      detail = ec.message();
      return "write-failed";
    }
    return {};
  }
  const auto dir = change->path.parent_path();
  if (!ensurePrivateDir(sshDir()) || !ensurePrivateDir(dir)) {
    detail = "~/.ssh/config.d";
    return "ssh-config-foreign";
  }
  if (change->text->contains("    ControlMaster auto\n")) {
    // The master connections' sockets: private, as ssh requires.
    auto control = sshDir();
    for (const auto& part : std::filesystem::path(security_hw::kControlDir)) {
      control /= part;
      if (!ensurePrivateDir(control)) {
        detail = "~/.ssh/" + std::string(security_hw::kControlDir);
        return "ssh-config-foreign";
      }
    }
    ::chmod(control.c_str(), 0700);
  }
  if (!security_hw::files::replaceFile(change->path, *change->text, 0600)) {
    detail = change->path.string();
    return "write-failed";
  }
  return {};
}

std::string_view SecurityKeyManager::ensureInclude(std::string& detail) {
  // "Include config.d/*.conf" at the top of ~/.ssh/config, where it applies to every host, unless
  // the file includes config.d already. A symbolic link (home-manager's) is never written to.
  const auto path = sshDir() / "config";
  const auto text = sshConfigText();
  if (text.has_value() && security_hw::includesDropIns(*text)) {
    return {};
  }
  std::error_code ec;
  if (std::filesystem::is_symlink(path, ec)) {
    detail = "~/.ssh/config";
    return "ssh-config-link";
  }
  if (!text.has_value()) {
    detail = "~/.ssh/config";
    return "ssh-config-foreign";
  }
  mode_t mode = 0600;
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) {
    mode = st.st_mode & 0777;
  }
  if (!ensurePrivateDir(sshDir())
      || !security_hw::files::replaceFile(
          path, std::string(security_hw::kSshInclude) + "\n" + (text->empty() ? "" : "\n" + *text), mode
      )) {
    detail = path.string();
    return "write-failed";
  }
  return {};
}

void SecurityKeyManager::unlinkPair(const std::string& name) {
  std::error_code ec;
  std::filesystem::remove(sshDir() / name, ec);
  std::filesystem::remove(sshDir() / (name + ".pub"), ec);
}

void SecurityKeyManager::installPairs(
    const std::filesystem::path& workdir, const std::vector<std::string>& names, std::vector<std::string>& imported,
    std::vector<std::string>& skipped
) {
  const auto dir = sshDir();
  if (!ensurePrivateDir(dir)) {
    skipped = names;
    return;
  }
  std::error_code ec;
  for (const auto& name : names) {
    const std::array<std::string, 2> pair{name, name + ".pub"};
    if (!std::ranges::all_of(pair, [&](const std::string& n) {
          return std::filesystem::is_regular_file(workdir / n, ec);
        })) {
      continue;
    }
    if (std::ranges::any_of(pair, [&](const std::string& n) {
          return std::filesystem::exists(std::filesystem::symlink_status(dir / n, ec));
        })) {
      skipped.push_back(name);
      continue;
    }
    const auto key = security_hw::files::readLimited(workdir / pair[0], false, kSshFileLimit);
    const auto pub = security_hw::files::readLimited(workdir / pair[1], false, kSshFileLimit);
    bool ok = key.has_value() && pub.has_value() && security_hw::files::writeNewFile(dir / pair[0], *key, 0600);
    if (ok && !security_hw::files::writeNewFile(dir / pair[1], *pub, 0644)) {
      std::filesystem::remove(dir / pair[0], ec);
      ok = false;
    }
    if (!ok) {
      kLog.warn("could not copy {} into {}", name, dir.string());
      skipped.push_back(name);
      continue;
    }
    imported.push_back(name);
  }
}

std::optional<std::filesystem::path> SecurityKeyManager::makeWorkdir(const std::string& pin) {
  std::string tmpl = (std::filesystem::path(runtimeDir()) / "noctalia-security-keys-XXXXXX").string();
  if (::mkdtemp(tmpl.data()) == nullptr) {
    return std::nullopt;
  }
  std::filesystem::path dir(tmpl);
  m_workdir = dir;
  if (!security_hw::files::writeNewFile(dir / "askpass", kAskpassScript, 0700)) {
    return std::nullopt;
  }
  if (!pin.empty() && !security_hw::files::writeNewFile(dir / "pin", pin + "\n", 0600)) {
    return std::nullopt;
  }
  return dir;
}

std::vector<process::EnvOverride> SecurityKeyManager::askpassEnv() const {
  std::vector<process::EnvOverride> env{
      {.name = "SSH_ASKPASS", .value = (*m_workdir / "askpass").string()},
      {.name = "SSH_ASKPASS_REQUIRE", .value = "force"},
  };
  if (std::getenv("DISPLAY") == nullptr) {
    env.push_back({.name = "DISPLAY", .value = ":0"});
  }
  return env;
}

void SecurityKeyManager::requestSshImport() {
  if (busy() || device() == nullptr) {
    return;
  }
  if (device()->clientPin) {
    m_pinPrompt = PinPrompt{.op = Op::SshImport};
    notifyChanged();
    return;
  }
  runSshImport({});
}

void SecurityKeyManager::runSshImport(const std::string& pin) {
  if (!beginOp(Op::SshImport, Area::Ssh)) {
    return;
  }
  if (device() == nullptr) {
    fail(Area::Ssh, "no-device");
    finishOp();
    return;
  }
  const auto workdir = makeWorkdir(pin);
  if (!workdir) {
    fail(Area::Ssh, "workdir", runtimeDir());
    finishOp();
    return;
  }
  setStage("touch");
  const bool started =
      run({"ssh-keygen", "-K", "-N", ""},
          [this, workdir = *workdir, pinGiven = !pin.empty()](const process::RunResult& result) {
            if (cancelled()) {
              finishOp();
              return;
            }
            std::vector<std::string> found;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(workdir, ec)) {
              const std::string name = entry.path().filename().string();
              if (security_hw::isResidentKeyName(name)) {
                found.push_back(name);
              }
            }
            std::ranges::sort(found);
            if (!result && found.empty()) {
              const std::string code = result.timedOut ? "timeout" : security_hw::fidoErrorCode(result.err, pinGiven);
              if (!reaskPin(Op::SshImport, code)) {
                fail(Area::Ssh, code, lastLine(result.err));
              }
              finishOp();
              // The key may have been pulled out, or its PIN retries changed.
              refresh();
              return;
            }
            std::vector<std::string> imported;
            std::vector<std::string> skipped;
            installPairs(workdir, found, imported, skipped);
            if (imported.empty() && skipped.empty()) {
              fail(Area::Ssh, "ssh-none");
            } else {
              const auto list = [](const std::vector<std::string>& names) {
                std::string out;
                for (const auto& name : names) {
                  out += (out.empty() ? "~/.ssh/" : ", ~/.ssh/") + name;
                }
                return out;
              };
              std::string message;
              if (!imported.empty()) {
                message = i18n::tr("settings.security-keys.ssh.imported", "names", list(imported));
              }
              if (!skipped.empty()) {
                message += (message.empty() ? "" : "\n")
                    + i18n::tr("settings.security-keys.ssh.skipped", "names", list(skipped));
              }
              succeed(Area::Ssh, std::move(message));
            }
            loadSsh();
            finishOp();
          },
          {.timeoutSeconds = kTouchTimeoutSeconds, .env = askpassEnv(), .workingDir = workdir->string()});
  if (!started) {
    fail(Area::Ssh, "start-failed", "ssh-keygen");
    finishOp();
  }
}

void SecurityKeyManager::requestSshCreate(SshCreateOptions options) {
  if (busy() || device() == nullptr) {
    return;
  }
  m_pendingCreate = std::move(options);
  m_sshReplace.reset();
  if (device()->clientPin) {
    m_pinPrompt = PinPrompt{.op = Op::SshCreate};
    notifyChanged();
    return;
  }
  runSshCreate({});
}

void SecurityKeyManager::runSshCreate(const std::string& pin) {
  if (!m_pendingCreate.has_value() || !beginOp(Op::SshCreate, Area::Ssh)) {
    return;
  }
  const SshCreateOptions options = *m_pendingCreate;
  if (const auto problem = security_hw::sshCreateProblem(options); !problem.empty()) {
    fail(Area::Ssh, "ssh-invalid", problem);
    finishOp();
    return;
  }
  if (device() == nullptr) {
    fail(Area::Ssh, "no-device");
    finishOp();
    return;
  }
  const std::string name = security_hw::residentKeyName(options.algorithm, options.application, options.user);
  std::error_code ec;
  for (const auto& n : {name, name + ".pub"}) {
    if (std::filesystem::exists(std::filesystem::symlink_status(sshDir() / n, ec))) {
      fail(Area::Ssh, "ssh-file-exists", "~/.ssh/" + name);
      finishOp();
      return;
    }
  }
  const auto workdir = makeWorkdir(pin);
  if (!workdir) {
    fail(Area::Ssh, "workdir", runtimeDir());
    finishOp();
    return;
  }
  setStage("touch");
  const bool started =
      run(security_hw::sshCreateCommand((*workdir / name).string(), options),
          [this, workdir = *workdir, name, options, pinGiven = !pin.empty()](const process::RunResult& result) {
            if (cancelled()) {
              finishOp();
              return;
            }
            std::error_code madeEc;
            const bool made = std::filesystem::exists(workdir / name, madeEc)
                && std::filesystem::exists(workdir / (name + ".pub"), madeEc);
            if (!result || !made) {
              if ((result.out + result.err).contains("Overwrite key in token")) {
                m_sshReplace = options;
                fail(Area::Ssh, "ssh-exists", "ssh:" + options.application);
                finishOp();
                return;
              }
              const std::string code = result.timedOut ? "timeout" : security_hw::fidoErrorCode(result.err, pinGiven);
              if (!reaskPin(Op::SshCreate, code)) {
                fail(Area::Ssh, code, lastLine(result.err));
              }
              finishOp();
              refresh();
              return;
            }
            std::vector<std::string> imported;
            std::vector<std::string> skipped;
            installPairs(workdir, {name}, imported, skipped);
            if (imported.empty()) {
              fail(Area::Ssh, "ssh-file-exists", "~/.ssh/" + name);
              finishOp();
              return;
            }
            m_pendingCreate.reset();
            const auto pub = sshPublic(name);
            const auto remote = pub ? security_hw::gitRemote(pub->application) : std::nullopt;
            const auto text = readSsh(name + ".pub", false);
            m_sshCreated = SshCreated{
                .name = name,
                .publicKey = text ? lastLine(*text) : std::string{},
                .host = remote ? remote->host : std::string{},
            };
            loadSsh();
            finishOp();
          },
          {
              // ssh-keygen asks on stdin before replacing a key on the security key.
              .stdinData = options.replace ? "y\n" : "n\n",
              .timeoutSeconds = kCreateTimeoutSeconds,
              .env = askpassEnv(),
              .workingDir = workdir->string(),
          });
  if (!started) {
    fail(Area::Ssh, "start-failed", "ssh-keygen");
    finishOp();
  }
}

void SecurityKeyManager::removeSshCopy(const std::string& name) {
  if (!beginOp(Op::SshRemove, Area::Ssh)) {
    return;
  }
  // Only names this page lists; the key itself stays on the security key.
  loadSsh();
  if (!std::ranges::any_of(m_sshKeys, [&](const SshKey& k) { return k.name == name; })) {
    fail(Area::Ssh, "remove-failed", name);
    finishOp();
    return;
  }
  const auto plan = hostChangeFor(name, false);
  if (!plan.code.empty()) {
    fail(Area::Ssh, plan.code, plan.detail);
    finishOp();
    return;
  }
  unlinkPair(name);
  std::string detail;
  if (const auto code = applyHost(plan.change, detail); !code.empty()) {
    fail(Area::Ssh, "remove-failed", detail);
  } else {
    succeed(Area::Ssh, i18n::tr("settings.security-keys.ssh.removed", "name", "~/.ssh/" + name));
  }
  loadSsh();
  finishOp();
}

void SecurityKeyManager::requestSshDelete(const std::string& name) {
  if (busy() || device() == nullptr) {
    return;
  }
  // Deleting a credential always asks for the PIN (fido2-token reads it on stdin).
  m_pendingName = name;
  m_pinPrompt = PinPrompt{.op = Op::SshDelete};
  notifyChanged();
}

void SecurityKeyManager::runSshDelete(const std::string& pin) {
  if (!beginOp(Op::SshDelete, Area::Ssh)) {
    return;
  }
  const std::string name = m_pendingName;
  loadSsh();
  if (!std::ranges::any_of(m_sshKeys, [&](const SshKey& k) { return k.name == name; })) {
    fail(Area::Ssh, "remove-failed", name);
    finishOp();
    return;
  }
  if (device() == nullptr) {
    fail(Area::Ssh, "no-device");
    finishOp();
    return;
  }
  const auto text = readSsh(name, false);
  const auto handle = text ? security_hw::parseSkHandle(*text) : std::nullopt;
  const auto pub = sshPublic(name);
  if (!handle || !pub || handle->blob != pub->blob) {
    fail(Area::Ssh, "ssh-damaged", "~/.ssh/" + name);
    finishOp();
    return;
  }
  if (handle->encrypted) {
    fail(Area::Ssh, "ssh-encrypted", "~/.ssh/" + name);
    finishOp();
    return;
  }
  // Checked before anything is deleted, so nothing is left half done.
  const auto plan = hostChangeFor(name, false);
  if (!plan.code.empty()) {
    fail(Area::Ssh, plan.code, plan.detail);
    finishOp();
    return;
  }
  const std::string devicePath = device()->path;
  setStage("touch");
  // Without a terminal fido2-token reads the PIN from stdin, once per run.
  const bool started =
      run({"fido2-token", "-L", "-k", pub->application, devicePath},
          [this, name, pin, devicePath, credential = handle->credential,
           change = plan.change](const process::RunResult& listing) {
            if (cancelled()) {
              finishOp();
              return;
            }
            if (!listing && !listing.err.contains("NO_CREDENTIALS")) {
              const std::string code = listing.timedOut ? "timeout" : security_hw::fidoErrorCode(listing.err, true);
              if (!reaskPin(Op::SshDelete, code)) {
                fail(Area::Ssh, code, lastLine(listing.err));
              }
              finishOp();
              refresh();
              return;
            }
            std::vector<security_hw::TokenCredential> matches;
            for (auto& c : security_hw::parseTokenCredentials(listing.out)) {
              if (c.raw == credential) {
                matches.push_back(std::move(c));
              }
            }
            if (matches.size() != 1) {
              fail(Area::Ssh, "ssh-not-on-key", "~/.ssh/" + name);
              finishOp();
              return;
            }
            const bool deleting =
                run({"fido2-token", "-D", "-i", matches.front().id, devicePath},
                    [this, name, change](const process::RunResult& result) {
                      if (cancelled()) {
                        finishOp();
                        return;
                      }
                      if (!result) {
                        const std::string code =
                            result.timedOut ? "timeout" : security_hw::fidoErrorCode(result.err, true);
                        if (!reaskPin(Op::SshDelete, code)) {
                          fail(Area::Ssh, code, lastLine(result.err));
                        }
                        finishOp();
                        refresh();
                        return;
                      }
                      // The key is gone from the security key: its copy no longer works.
                      unlinkPair(name);
                      std::string detail;
                      if (const auto code = applyHost(change, detail); !code.empty()) {
                        kLog.warn("cleaning up after deleting {} failed: {}", name, detail);
                      }
                      succeed(Area::Ssh, i18n::tr("settings.security-keys.ssh.deleted", "name", "~/.ssh/" + name));
                      loadSsh();
                      finishOp();
                    },
                    {.stdinData = pin + "\n", .timeoutSeconds = kAuthTimeoutSeconds});
            if (!deleting) {
              fail(Area::Ssh, "start-failed", "fido2-token");
              finishOp();
            }
          },
          {.stdinData = pin + "\n", .timeoutSeconds = kAuthTimeoutSeconds});
  if (!started) {
    fail(Area::Ssh, "start-failed", "fido2-token");
    finishOp();
  }
}

void SecurityKeyManager::setSshHost(const std::string& name, bool enable) {
  if (!beginOp(Op::SshHost, Area::Ssh)) {
    return;
  }
  loadSsh();
  const auto it = std::ranges::find_if(m_sshKeys, [&](const SshKey& k) { return k.name == name; });
  const auto remote = it != m_sshKeys.end() ? security_hw::gitRemote(it->application) : std::nullopt;
  if (!remote) {
    fail(Area::Ssh, "generic", name);
    finishOp();
    return;
  }
  const auto plan = planHost(name, *remote, enable, std::nullopt);
  std::string detail = plan.detail;
  std::string code = plan.code;
  if (code.empty() && enable) {
    code = ensureInclude(detail);
  }
  if (code.empty()) {
    code = applyHost(plan.change, detail);
  }
  if (!code.empty()) {
    fail(Area::Ssh, code, detail);
  } else {
    succeed(
        Area::Ssh,
        i18n::tr(
            enable ? "settings.security-keys.ssh.host-enabled" : "settings.security-keys.ssh.host-disabled", "host",
            remote->host
        )
    );
  }
  loadSsh();
  finishOp();
}

void SecurityKeyManager::setSshPersist(const std::string& name, int minutes) {
  if (!beginOp(Op::SshPersist, Area::Ssh)) {
    return;
  }
  loadSsh();
  const auto it = std::ranges::find_if(m_sshKeys, [&](const SshKey& k) { return k.name == name; });
  const auto remote = it != m_sshKeys.end() ? security_hw::gitRemote(it->application) : std::nullopt;
  if (!security_hw::validPersistMinutes(minutes)) {
    fail(Area::Ssh, "ssh-invalid", "minutes");
  } else if (!remote || it->hostState != HostState::On) {
    fail(Area::Ssh, "generic", name);
  } else {
    const auto plan = planHost(name, *remote, true, minutes);
    std::string detail = plan.detail;
    std::string code = plan.code;
    if (code.empty()) {
      code = applyHost(plan.change, detail);
    }
    if (!code.empty()) {
      fail(Area::Ssh, code, detail);
    } else if (minutes == 0) {
      succeed(Area::Ssh, i18n::tr("settings.security-keys.ssh.persist-none", "host", remote->host));
    } else {
      succeed(
          Area::Ssh,
          i18n::tr("settings.security-keys.ssh.persist-set", "host", remote->host, "minutes", std::to_string(minutes))
      );
    }
  }
  loadSsh();
  finishOp();
}
