#include "security/security_hardware_parsers.h"
#include "util/base64.h"

#include <cstdio>
#include <print>
#include <sodium.h>
#include <string>
#include <string_view>
#include <vector>

using namespace security_hw;

namespace {

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "security_hardware_parsers_test: {}", message);
    }
    return condition;
  }

  bool expectEqual(const std::string& actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
      std::println(stderr, "security_hardware_parsers_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

  // Resident SSH keys, built the way ssh-keygen writes them (no key needed).
  constexpr std::string_view kEd25519Sk = "sk-ssh-ed25519@openssh.com";

  void append(Bytes& out, std::string_view data) { out.insert(out.end(), data.begin(), data.end()); }

  void appendString(Bytes& out, std::string_view data) {
    const auto size = static_cast<std::uint32_t>(data.size());
    out.push_back(static_cast<std::uint8_t>(size >> 24U));
    out.push_back(static_cast<std::uint8_t>(size >> 16U));
    out.push_back(static_cast<std::uint8_t>(size >> 8U));
    out.push_back(static_cast<std::uint8_t>(size));
    append(out, data);
  }

  void appendString(Bytes& out, const Bytes& data) {
    appendString(out, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
  }

  Bytes skBlob(std::string_view application) {
    Bytes blob;
    appendString(blob, kEd25519Sk);
    appendString(blob, std::string(32, '\x01'));
    appendString(blob, application);
    return blob;
  }

  std::string pubText(std::string_view application) {
    return std::string(kEd25519Sk) + " " + Base64::encode(skBlob(application)) + " ada@host\n";
  }

  std::string handleText(std::string_view application, std::string_view credential, std::string_view cipher = "none") {
    Bytes priv;
    append(priv, std::string_view("\0\0\0\7\0\0\0\7", 8));
    appendString(priv, kEd25519Sk);
    appendString(priv, std::string(32, '\x01'));
    appendString(priv, application);
    priv.push_back(0x01);
    appendString(priv, credential);
    appendString(priv, "");
    appendString(priv, "ada@host");
    for (std::uint8_t pad = 1; priv.size() % 8 != 0; ++pad) {
      priv.push_back(pad);
    }
    Bytes data;
    append(data, std::string_view("openssh-key-v1\0", 15));
    appendString(data, cipher);
    appendString(data, cipher == "none" ? "none" : "bcrypt");
    appendString(data, "");
    append(data, std::string_view("\0\0\0\1", 4));
    appendString(data, skBlob(application));
    appendString(data, priv);
    // A synthetic key handle, never a real key; the armor is split so secret scanners don't flag it.
    const std::string armor = std::string("OPENSSH ") + "PRIVATE KEY-----";
    return "-----BEGIN " + armor + "\n" + Base64::encode(data) + "\n-----END " + armor + "\n";
  }

  bool fido() {
    bool ok = true;
    auto devices =
        parseFidoDevices("/dev/hidraw1: vendor=0x1050, product=0x0407 (Yubico YubiKey OTP+FIDO+CCID)\ngarbage\n");
    ok = expect(devices.size() == 1, "one device") && ok;
    if (devices.size() == 1) {
      ok = expectEqual(devices[0].path, "/dev/hidraw1", "device path") && ok;
      ok = expectEqual(devices[0].name, "Yubico YubiKey OTP+FIDO+CCID", "device name") && ok;
      applyFidoInfo(
          devices[0],
          "proto: 0x02\nversion strings: U2F_V2, FIDO_2_0\noptions: rk, up, noplat, clientPin, "
          "credentialMgmtPreview\npin retries: 8\n"
      );
      ok = expect(devices[0].residentKeys && devices[0].clientPin && devices[0].pinRetries == 8, "device info") && ok;
      applyFidoInfo(devices[0], "options: up, noclientPin\n");
      ok = expect(!devices[0].residentKeys && !devices[0].clientPin && devices[0].pinRetries == -1, "no pin") && ok;
    }

    const auto pam = parsePamU2f("auth sufficient pam_u2f.so cue authfile=/etc/fido2/fido2\n");
    ok = expect(pam.u2f && pam.authfile == "/etc/fido2/fido2" && !pam.pin, "pam authfile") && ok;
    ok = expect(!parsePamU2f("#auth sufficient pam_u2f.so\nauth include system-auth\n").u2f, "pam comment") && ok;
    ok = expect(parsePamU2f("auth sufficient pam_u2f.so cue pinverification=1 userpresence=1\n").pin, "pam pin") && ok;
    ok = expect(!parsePamU2f("auth sufficient pam_u2f.so cue pinverification=0\n").pin, "pam no pin") && ok;
    ok = expect(parsePamU2f("auth sufficient pam_u2f.so cue pinverification=0\n").pinOff, "pam pin off") && ok;
    ok = expect(!parsePamU2f("auth sufficient pam_u2f.so cue\n").pinOff, "pam pin unset") && ok;

    // +pin on a credential: what pam_u2f honors when the PAM line leaves pinverification unset.
    ok = expect(!credentialRequiresPin("KH,PK,es256,+presence"), "cred no pin") && ok;
    ok = expect(credentialRequiresPin("KH,PK,es256,+presence+pin"), "cred pin") && ok;
    ok = expect(!credentialRequiresPin("KH,PK,es256,+presence+pinned"), "cred pin prefix") && ok;
    ok = expect(!credentialRequiresPin("KH,PK"), "cred old format") && ok;
    ok = expect(credentialWithPin("KH,PK,es256,+presence", true) == "KH,PK,es256,+presence+pin", "add pin") && ok;
    ok = expect(credentialWithPin("KH,PK,es256,+presence+pin", true) == "KH,PK,es256,+presence+pin", "pin kept once")
        && ok;
    ok = expect(credentialWithPin("KH,PK,es256,+pin+presence", false) == "KH,PK,es256,+presence", "drop pin") && ok;
    ok = expect(credentialWithPin("KH,PK,es256,", true) == "KH,PK,es256,+pin", "pin on empty options") && ok;
    ok = expect(credentialWithPin("KH,PK", true) == "KH,PK", "old format unchanged") && ok;

    const std::string own = "/home/ada/.config/Yubico/u2f_keys";
    const PamU2f mine{.u2f = true};
    const PamU2f other{.u2f = true, .authfile = "/etc/fido2/fido2"};
    ok = expect(fidoTarget({mine, mine}, own).kind == FidoTargetKind::Own, "target own") && ok;
    ok = expect(fidoTarget({mine, {}}, own).kind == FidoTargetKind::Own, "target own, one service") && ok;
    ok = expect(fidoTarget({{}, {}}, own).kind == FidoTargetKind::NoPam, "target no pam") && ok;
    ok = expect(fidoTarget({other, other}, own).kind == FidoTargetKind::OtherFile, "target other file") && ok;
    ok = expect(fidoTarget({mine, other}, own).kind == FidoTargetKind::Mixed, "target mixed") && ok;
    ok = expect(pamUsesFile({mine, other}, own, own) && !pamUsesFile({other}, own, own), "pam uses file") && ok;

    const std::string cred1 = "HANDLEONE111,pubkey1,es256,+presence";
    const std::string cred2 = "HANDLETWO222,pubkey2,es256,+presence";
    const auto merged = mergeCredentials({cred1}, {"HANDLEONE111,other,es256,+presence", cred2});
    ok = expect(merged == std::vector<std::string>{cred1, cred2}, "merge skips known key handles") && ok;
    ok = expect(credentialHandles("ada:" + cred1 + ":" + cred2, "ada").size() == 2, "credential handles") && ok;
    ok = expect(credentialHandles("adam:" + cred1, "ada").empty(), "another user's line") && ok;
    ok = expectEqual(credentialLabel(cred1), "HANDLEONE1", "credential label") && ok;

    ok = expectEqual(fidoErrorCode("error: fido_dev_make_cred (49) FIDO_ERR_PIN_INVALID", false), "pin-invalid", "e1")
        && ok;
    ok = expectEqual(fidoErrorCode("error: fido_dev_make_cred (50) FIDO_ERR_PIN_BLOCKED", false), "pin-blocked", "e2")
        && ok;
    ok = expectEqual(fidoErrorCode("FIDO_ERR_ACTION_TIMEOUT", false), "timeout", "e3") && ok;
    ok = expectEqual(fidoErrorCode("Unable to load resident keys: PIN incorrect", false), "pin-invalid", "e4") && ok;
    ok = expectEqual(fidoErrorCode("No keys to download", false), "ssh-none", "e5") && ok;
    ok = expectEqual(fidoErrorCode("something odd", false), "generic", "e6") && ok;
    const std::string empty = "Enter PIN for /dev/hidraw1: error: fido_dev_make_cred (-7) FIDO_ERR_INVALID_ARGUMENT";
    ok = expectEqual(fidoErrorCode(empty, false), "pin-required", "empty pin") && ok;
    ok = expectEqual(fidoErrorCode(empty, true), "generic", "pin given") && ok;
    return ok;
  }

  bool gpg() {
    bool ok = true;
    // Field 15 (index 14): "+" or empty for a secret on disk, the card serial or "#" for a stub.
    const auto record = [](std::string_view kind, std::string_view token) {
      return std::string(kind) + ":u:255:22:X:1:::::::::" + std::string(token) + ":::\n";
    };
    ok = expect(secretOnDisk(record("sec", "+")), "secret on disk") && ok;
    ok =
        expect(secretOnDisk(record("sec", "D2760001240100000006203732350000") + record("ssb", "")), "one secret") && ok;
    ok = expect(!secretOnDisk(record("sec", "D2760001240100000006203732350000") + record("ssb", "#")), "stubs only")
        && ok;
    ok = expectEqual(
             formatFingerprint("aaaabbbbccccddddeeeeffff0000111122223333"),
             "AAAA BBBB CCCC DDDD EEEE  FFFF 0000 1111 2222 3333", "fingerprint format"
         )
        && ok;
    return ok;
  }

  bool ssh() {
    bool ok = true;
    const auto pub = parseSkPublic(pubText("ssh:github.com"));
    ok = expect(pub.has_value() && pub->type == "ed25519-sk" && pub->application == "ssh:github.com", "public") && ok;
    ok = expect(!parseSkPublic("ssh-ed25519 AAAA x").has_value(), "not a security key") && ok;

    const auto handle = parseSkHandle(handleText("ssh:github.com", std::string(48, 'K')));
    ok = expect(handle.has_value() && !handle->encrypted, "handle") && ok;
    if (handle.has_value() && pub.has_value()) {
      ok = expect(handle->credential == Bytes(48, 'K'), "credential id") && ok;
      ok = expect(handle->blob == pub->blob, "handle blob matches the public key") && ok;
    }
    const auto encrypted = parseSkHandle(handleText("ssh:github.com", std::string(48, 'K'), "aes256-ctr"));
    ok = expect(encrypted.has_value() && encrypted->encrypted && encrypted->credential.empty(), "encrypted") && ok;
    ok = expect(!parseSkHandle("not a key").has_value(), "not a key") && ok;

    const Bytes raw(48, 'K');
    const std::string id = Base64::encode(raw);
    const auto creds = parseTokenCredentials("00: " + id + " (null) dXNlcg== eddsa uvopt+id\nbad line\n");
    ok = expect(creds.size() == 1 && creds[0].raw == raw && creds[0].id == id, "token credentials") && ok;

    ok = expect(sshFingerprint(Bytes{}) == "SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU", "fingerprint") && ok;

    const auto github = gitRemote("ssh:github.com");
    ok = expect(github.has_value() && *github == GitRemote{.login = "git", .host = "github.com", .port = ""}, "github")
        && ok;
    const auto custom = gitRemote("ssh:me@git.example.org:2222");
    ok = expect(
             custom.has_value() && *custom == GitRemote{.login = "me", .host = "git.example.org", .port = "2222"},
             "custom"
         )
        && ok;
    for (const std::string_view application :
         {"ssh:", "ssh:work", "ssh:example.org", "ssh:github.com:99999", "github.com"}) {
      ok = expect(!gitRemote(application).has_value(), application) && ok;
    }

    const SshCreateOptions create{
        .algorithm = "ed25519-sk",
        .application = "github.com",
        .user = "0123456789abcdef",
        .comment = "ada@host",
    };
    ok = expectEqual(sshCreateProblem(create), "", "valid options") && ok;
    auto bad = [&](auto change, std::string_view expected) {
      SshCreateOptions options = create;
      change(options);
      return expectEqual(sshCreateProblem(options), expected, "create problem");
    };
    ok = bad([](SshCreateOptions& o) { o.comment = "café"; }, "comment") && ok;
    ok = bad([](SshCreateOptions& o) { o.comment = std::string(129, 'a'); }, "comment") && ok;
    ok = bad([](SshCreateOptions& o) { o.user = "work.pub"; }, "pub-name") && ok;
    ok = bad([](SshCreateOptions& o) { o.user = "a/b"; }, "user") && ok;
    ok = bad([](SshCreateOptions& o) { o.user = "a..b"; }, "user") && ok;
    ok = bad([](SshCreateOptions& o) { o.application = "x/y"; }, "application") && ok;
    ok = bad(
             [](SshCreateOptions& o) {
               o.application = "";
               o.user = "x.pub";
             },
             "pub-name"
         )
        && ok;
    ok = bad([](SshCreateOptions& o) { o.algorithm = "rsa"; }, "algorithm") && ok;
    ok = bad(
             [](SshCreateOptions& o) {
               o.application = "";
               o.user = "";
             },
             ""
         )
        && ok;

    ok = expectEqual(residentKeyName("ed25519-sk", "github.com", "work"), "id_ed25519_sk_rk_github.com_work", "name")
        && ok;
    ok = expectEqual(residentKeyName("ecdsa-sk", "", ""), "id_ecdsa_sk_rk", "bare name") && ok;
    ok = expect(
             isResidentKeyName("id_ed25519_sk_rk_github.com")
                 && !isResidentKeyName("id_ed25519_sk_rk.pub")
                 && !isResidentKeyName("id_ed25519"),
             "resident names"
         )
        && ok;
    ok = expectEqual(residentKeyUser("id_ed25519_sk_rk_github.com_work", "ed25519-sk", "ssh:github.com"), "work", "u1")
        && ok;
    ok = expectEqual(residentKeyUser("id_ed25519_sk_rk_github.com", "ed25519-sk", "ssh:github.com"), "", "u2") && ok;
    ok = expectEqual(residentKeyUser("id_ecdsa_sk_rk_abc", "ecdsa-sk", "ssh:"), "abc", "u3") && ok;
    ok = expectEqual(residentKeyUser("id_ed25519_sk_rk_other_x", "ed25519-sk", "ssh:github.com"), "", "u4") && ok;

    const auto command = sshCreateCommand("/run/x/id", SshCreateOptions{.application = "github.com", .touch = false});
    const std::vector<std::string> expected{
        "ssh-keygen",
        "-t",
        "ed25519-sk",
        "-O",
        "resident",
        "-O",
        "application=ssh:github.com",
        "-O",
        "verify-required",
        "-O",
        "no-touch-required",
        "-C",
        "",
        "-N",
        "",
        "-f",
        "/run/x/id",
    };
    ok = expect(command == expected, "create command") && ok;
    return ok;
  }

  bool dropIns() {
    bool ok = true;
    const GitRemote github{.login = "git", .host = "github.com", .port = ""};
    const std::vector<std::string> names{"id_b", "id_a"};

    const std::string off = dropInText(names, github, 0);
    ok = expect(off.contains("    IdentityFile ~/.ssh/id_a\n    IdentityFile ~/.ssh/id_b\n"), "sorted") && ok;
    ok = expect(off.contains("    ControlPersist no\n"), "no reuse") && ok;
    auto parsed = parseDropIn(off);
    ok = expect(parsed.kind == DropInKind::Ours && parsed.minutes == 0 && parsed.names.size() == 2, "ours") && ok;

    const std::string reuse = dropInText(names, github, 30);
    ok = expect(reuse.contains("    ControlPersist 30m\n"), "reuse") && ok;
    ok = expect(reuse.contains("    ControlPath ~/.ssh/noctalia/ssh-control/%C\n"), "path") && ok;
    parsed = parseDropIn(reuse);
    ok = expect(parsed.kind == DropInKind::Ours && parsed.minutes == 30, "ours with reuse") && ok;

    // No Control lines: still ours, reported as no reuse.
    const std::string bare = dropInText(names, github, std::nullopt);
    ok = expect(!bare.contains("Control"), "no control lines") && ok;
    parsed = parseDropIn(bare);
    ok = expect(parsed.kind == DropInKind::Ours && !parsed.minutes.has_value(), "ours without control lines") && ok;

    // A hand-edited duration is no longer ours.
    std::string edited = reuse;
    edited.replace(edited.find("30m"), 3, "2h");
    ok = expect(parseDropIn(edited).kind == DropInKind::Edited, "edited") && ok;
    ok = expect(parseDropIn("Host github.com\n    User git\n").kind == DropInKind::Foreign, "foreign") && ok;

    // A non-default port is kept.
    const std::string withPort =
        "# Written by Noctalia (Settings > Security); remove this line to keep your edits.\nHost gitlab.com\n    User "
        "git\n    Port 2222\n    IdentityFile ~/.ssh/id_x\n    IdentitiesOnly "
        "yes\n    IdentityAgent none\n    ControlMaster no\n    ControlPath none\n    ControlPersist no\n";
    parsed = parseDropIn(withPort);
    ok = expect(parsed.kind == DropInKind::Ours && parsed.remote.port == "2222" && parsed.minutes == 0, "port") && ok;

    ok = expect(includesDropIns("Host *\n  Include ~/.ssh/config.d/*.conf\n"), "home-manager include") && ok;
    ok = expect(includesDropIns("include config.d/*.conf\n"), "include") && ok;
    ok = expect(!includesDropIns("Host *\n  IdentityFile x\n"), "no include") && ok;
    ok = expect(validPersistMinutes(0) && validPersistMinutes(1440) && !validPersistMinutes(1441), "minutes") && ok;
    return ok;
  }

} // namespace

int main() {
  if (sodium_init() < 0) {
    return 1;
  }
  bool ok = fido();
  ok = gpg() && ok;
  ok = ssh() && ok;
  ok = dropIns() && ok;
  return ok ? 0 : 1;
}
