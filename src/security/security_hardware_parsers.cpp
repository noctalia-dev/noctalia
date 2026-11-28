#include "security/security_hardware_parsers.h"

#include "util/base64.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <regex>
#include <set>
#include <sodium.h>

namespace security_hw {

  namespace {

    // The first line of every drop-in this page writes; a drop-in without it is left alone.
    constexpr std::string_view kDropInMarker =
        "# Written by Noctalia (Settings > Security); remove this line to keep your edits.";
    constexpr std::string_view kIdentity = "    IdentityFile ~/.ssh/";

    constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kSkKeyTypes{{
        {"sk-ssh-ed25519@openssh.com", "ed25519-sk"},
        {"sk-ecdsa-sha2-nistp256@openssh.com", "ecdsa-sk"},
    }};

    constexpr std::array<std::string_view, 8> kGitHosts{
        "github.com", "gitlab.com", "bitbucket.org",     "codeberg.org",
        "git.sr.ht",  "gitea.com",  "ssh.dev.azure.com", "vs-ssh.visualstudio.com",
    };

    // libfido2's error names, as pamu2fcfg, fido2-token and ssh-keygen print them.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 18> kFidoErrors{{
        {"PIN_INVALID", "pin-invalid"},
        {"PIN incorrect", "pin-invalid"},
        {"PIN_AUTH_BLOCKED", "pin-blocked"},
        {"PIN_BLOCKED", "pin-blocked"},
        {"Too many incorrect PINs", "pin-blocked"},
        {"PIN_REQUIRED", "pin-required"},
        {"pin required", "pin-required"},
        {"failed to read pin", "pin-required"},
        {"couldn't read PIN", "pin-required"},
        {"ACTION_TIMEOUT", "timeout"},
        {"USER_ACTION_TIMEOUT", "timeout"},
        {"No device found", "no-device"},
        {"No FIDO SecurityKeyProvider", "no-device"},
        {"device not found", "no-device"},
        {"fido_dev_open", "no-device"},
        {"No keys to download", "ssh-none"},
        {"no keys found", "ssh-none"},
        {"returned no keys", "ssh-none"},
    }};

    using StringUtils::splitWhitespace;

    std::string_view trim(std::string_view text) { return StringUtils::trimRightView(StringUtils::trimLeftView(text)); }

    // Python's str.splitlines() for the texts here: every line, empty ones included. The views
    // point into <text>.
    std::vector<std::string_view> allLines(std::string_view text) {
      if (text.ends_with('\n')) {
        text.remove_suffix(1);
      }
      std::vector<std::string_view> out;
      if (text.empty()) {
        return out;
      }
      for (auto line : StringUtils::split(text, '\n')) {
        if (line.ends_with('\r')) {
          line.remove_suffix(1);
        }
        out.push_back(line);
      }
      return out;
    }

    std::optional<std::string_view> skKeyType(std::string_view name) {
      for (const auto& [wire, type] : kSkKeyTypes) {
        if (wire == name) {
          return type;
        }
      }
      return std::nullopt;
    }

    std::string_view algorithmFilePart(std::string_view algorithm) {
      if (algorithm == "ed25519-sk") {
        return "ed25519_sk";
      }
      if (algorithm == "ecdsa-sk") {
        return "ecdsa_sk";
      }
      return {};
    }

    // Reads the SSH wire format's length-prefixed strings.
    class WireReader {
    public:
      explicit WireReader(const Bytes& data, std::size_t at = 0) : m_data(data), m_at(at) {}

      std::optional<Bytes> string() {
        if (m_at + 4 > m_data.size()) {
          return std::nullopt;
        }
        const std::size_t length = (static_cast<std::size_t>(m_data[m_at]) << 24U)
            | (static_cast<std::size_t>(m_data[m_at + 1]) << 16U)
            | (static_cast<std::size_t>(m_data[m_at + 2]) << 8U)
            | static_cast<std::size_t>(m_data[m_at + 3]);
        const std::size_t begin = m_at + 4;
        if (length > m_data.size() - begin) {
          return std::nullopt;
        }
        m_at = begin + length;
        return Bytes(
            m_data.begin() + static_cast<std::ptrdiff_t>(begin), m_data.begin() + static_cast<std::ptrdiff_t>(m_at)
        );
      }

      bool skip(std::size_t count) {
        if (count > m_data.size() - m_at) {
          return false;
        }
        m_at += count;
        return true;
      }

      std::size_t position() const noexcept { return m_at; }
      bool atEnd() const noexcept { return m_at == m_data.size(); }

    private:
      const Bytes& m_data;
      std::size_t m_at;
    };

    std::string asText(const Bytes& bytes) { return {bytes.begin(), bytes.end()}; }

    bool isAscii(const Bytes& bytes) {
      return std::ranges::all_of(bytes, [](std::uint8_t c) { return c < 0x80; });
    }

    std::string dropInWith(
        std::string_view marker, const std::vector<std::string>& names, const GitRemote& remote,
        std::optional<int> minutes
    ) {
      std::vector<std::string> sorted = names;
      std::ranges::sort(sorted);
      std::string text(marker);
      text += "\nHost " + remote.host + "\n    User " + remote.login + "\n";
      if (!remote.port.empty()) {
        text += "    Port " + remote.port + "\n";
      }
      for (const auto& name : sorted) {
        text += std::string(kIdentity) + name + "\n";
      }
      text += "    IdentitiesOnly yes\n    IdentityAgent none\n";
      if (minutes.has_value() && *minutes != 0) {
        text += "    ControlMaster auto\n    ControlPath ~/.ssh/"
            + std::string(kControlDir)
            + "/%C\n    ControlPersist "
            + std::to_string(*minutes)
            + "m\n";
      } else if (minutes.has_value()) {
        text += "    ControlMaster no\n    ControlPath none\n    ControlPersist no\n";
      }
      return text;
    }

    std::optional<int> parseInt(std::string_view text) {
      int value = 0;
      const auto* end = text.data() + text.size();
      const auto [ptr, ec] = std::from_chars(text.data(), end, value);
      if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
      }
      return value;
    }

  } // namespace

  std::vector<std::string_view> splitLines(std::string_view text) {
    auto lines = allLines(text);
    std::erase_if(lines, [](std::string_view line) { return line.empty(); });
    return lines;
  }

  std::string lastLine(std::string_view text) {
    std::string_view last;
    for (const auto line : allLines(text)) {
      if (const auto trimmed = trim(line); !trimmed.empty()) {
        last = trimmed;
      }
    }
    return std::string(last);
  }

  std::optional<Bytes> decodeBase64Strict(std::string_view text) {
    if (text.size() % 4 != 0) {
      return std::nullopt;
    }
    std::size_t padding = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (c == '=') {
        if (i + 2 < text.size()) {
          return std::nullopt;
        }
        ++padding;
        continue;
      }
      if (padding > 0 || !Base64::kChars.contains(c)) {
        return std::nullopt;
      }
    }
    return Base64::decode(text);
  }

  // -- FIDO2/U2F --------------------------------------------------------------------------------

  std::vector<FidoDevice> parseFidoDevices(std::string_view text) {
    std::vector<FidoDevice> devices;
    for (const auto& line : splitLines(text)) {
      const auto sep = line.find(": ");
      if (sep == std::string::npos || line.empty() || line.front() != '/') {
        continue;
      }
      FidoDevice device;
      device.path = line.substr(0, sep);
      const auto trimmed = trim(line.substr(sep + 2));
      const auto open = trimmed.find('(');
      if (!trimmed.empty() && trimmed.back() == ')' && open != std::string_view::npos) {
        device.name = std::string(trimmed.substr(open + 1, trimmed.size() - open - 2));
      } else {
        device.name = std::string(trimmed);
      }
      devices.push_back(std::move(device));
    }
    return devices;
  }

  void applyFidoInfo(FidoDevice& device, std::string_view text) {
    device.residentKeys = false;
    device.clientPin = false;
    device.pinRetries = -1;
    for (const auto& line : splitLines(text)) {
      const auto colon = line.find(':');
      const auto key = trim(std::string_view(line).substr(0, colon));
      const auto value = colon == std::string::npos ? std::string_view{} : std::string_view(line).substr(colon + 1);
      if (key == "options") {
        for (const auto option : StringUtils::split(value, ',')) {
          const auto name = trim(option);
          if (name == "rk") {
            device.residentKeys = true;
          } else if (name == "clientPin") {
            device.clientPin = true;
          }
        }
      } else if (key == "pin retries") {
        if (const auto retries = parseInt(trim(value))) {
          device.pinRetries = *retries;
        }
      }
    }
  }

  std::vector<std::string> credentialHandles(std::string_view line, std::string_view user) {
    std::vector<std::string> out;
    if (!line.starts_with(user) || line.size() <= user.size() || line[user.size()] != ':') {
      return out;
    }
    for (const auto credential : StringUtils::split(line.substr(user.size() + 1), ':')) {
      if (!credential.empty()) {
        out.emplace_back(credential);
      }
    }
    return out;
  }

  std::string credentialKeyHandle(std::string_view credential) {
    return std::string(credential.substr(0, credential.find(',')));
  }

  std::string credentialLabel(std::string_view credential) { return credentialKeyHandle(credential).substr(0, 10); }

  std::vector<std::string>
  mergeCredentials(const std::vector<std::string>& existing, const std::vector<std::string>& added) {
    std::vector<std::string> merged = existing;
    std::set<std::string> handles;
    for (const auto& credential : existing) {
      handles.insert(credentialKeyHandle(credential));
    }
    for (const auto& credential : added) {
      const auto handle = credentialKeyHandle(credential);
      if (!handle.empty() && handles.insert(handle).second) {
        merged.push_back(credential);
      }
    }
    return merged;
  }

  namespace {
    // "keyHandle,publicKey,coseType,options" → its fields.
    std::vector<std::string> credentialFields(std::string_view credential) {
      std::vector<std::string> fields;
      std::size_t start = 0;
      while (true) {
        const auto comma = credential.find(',', start);
        fields.emplace_back(credential.substr(start, comma == std::string_view::npos ? comma : comma - start));
        if (comma == std::string_view::npos) {
          return fields;
        }
        start = comma + 1;
      }
    }

    // "+presence+pin" → {"presence", "pin"}.
    std::vector<std::string> optionNames(std::string_view options) {
      std::vector<std::string> names;
      std::size_t start = 0;
      while (start < options.size()) {
        if (options[start] == '+') {
          ++start;
          continue;
        }
        const auto plus = options.find('+', start);
        names.emplace_back(options.substr(start, plus == std::string_view::npos ? plus : plus - start));
        if (plus == std::string_view::npos) {
          break;
        }
        start = plus;
      }
      return names;
    }
  } // namespace

  bool credentialRequiresPin(std::string_view credential) {
    const auto fields = credentialFields(credential);
    return fields.size() >= 4 && std::ranges::contains(optionNames(fields[3]), "pin");
  }

  std::string credentialWithPin(std::string_view credential, bool pin) {
    auto fields = credentialFields(credential);
    if (fields.size() < 4) {
      return std::string(credential);
    }
    std::string options;
    for (const auto& name : optionNames(fields[3])) {
      if (name != "pin") {
        options += "+" + name;
      }
    }
    if (pin) {
      options += "+pin";
    }
    fields[3] = std::move(options);
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
      out += (i == 0 ? "" : ",") + fields[i];
    }
    return out;
  }

  PamU2f parsePamU2f(std::string_view text) {
    for (const auto& raw : splitLines(text)) {
      const auto line = trim(raw);
      if (line.starts_with('#') || !line.contains("pam_u2f.so")) {
        continue;
      }
      PamU2f state{.u2f = true};
      for (const auto& word : splitWhitespace(line)) {
        if (word.starts_with("authfile=")) {
          state.authfile = word.substr(9);
        } else if (word == "pinverification=1") {
          state.pin = true;
        } else if (word == "pinverification=0") {
          state.pinOff = true;
        }
      }
      return state;
    }
    return {};
  }

  namespace {
    std::string normalizePath(std::string_view path) { return std::filesystem::path(path).lexically_normal().string(); }
  } // namespace

  bool pamUsesFile(const std::vector<PamU2f>& services, std::string_view path, std::string_view own) {
    const auto wanted = normalizePath(path);
    return std::ranges::any_of(services, [&](const PamU2f& s) {
      return s.u2f && normalizePath(s.authfile.empty() ? own : s.authfile) == wanted;
    });
  }

  FidoTarget fidoTarget(const std::vector<PamU2f>& services, std::string_view ownAuthfile) {
    std::set<std::string> paths;
    for (const auto& service : services) {
      if (service.u2f) {
        paths.insert(normalizePath(service.authfile.empty() ? ownAuthfile : service.authfile));
      }
    }
    if (paths.empty()) {
      return {.kind = FidoTargetKind::NoPam};
    }
    if (paths.size() > 1) {
      return {.kind = FidoTargetKind::Mixed};
    }
    const std::string path = *paths.begin();
    return {
        .kind = path == normalizePath(ownAuthfile) ? FidoTargetKind::Own : FidoTargetKind::OtherFile,
        .path = path,
    };
  }

  std::string fidoErrorCode(std::string_view text, bool pinGiven) {
    // Asked for a PIN it wasn't given (pamu2fcfg then fails on the empty one).
    if (!pinGiven && text.contains("Enter PIN")) {
      return "pin-required";
    }
    for (const auto& [needle, code] : kFidoErrors) {
      if (text.contains(needle)) {
        return std::string(code);
      }
    }
    return "generic";
  }

  // -- OpenPGP ----------------------------------------------------------------------------------

  bool secretOnDisk(std::string_view colons) {
    return std::ranges::any_of(splitLines(colons), [](std::string_view line) {
      const auto fields = StringUtils::split(line, ':');
      if (fields.front() != "sec" && fields.front() != "ssb") {
        return false;
      }
      const std::string_view token = fields.size() > 14 ? fields[14] : std::string_view{};
      return token.empty() || token == "+";
    });
  }

  std::string formatFingerprint(std::string_view fpr) {
    std::string hex(fpr);
    std::ranges::transform(hex, hex.begin(), [](char c) {
      return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    });
    if (hex.size() != 40) {
      return hex;
    }
    std::string out;
    for (std::size_t i = 0; i < 40; i += 4) {
      if (i == 20) {
        out += "  ";
      } else if (i > 0) {
        out += ' ';
      }
      out += hex.substr(i, 4);
    }
    return out;
  }

  // -- Resident SSH keys ------------------------------------------------------------------------

  std::optional<SkPublic> parseSkPublic(std::string_view text) {
    const auto fields = splitWhitespace(text);
    if (fields.size() < 2) {
      return std::nullopt;
    }
    const auto declared = skKeyType(fields[0]);
    if (!declared) {
      return std::nullopt;
    }
    auto blob = decodeBase64Strict(fields[1]);
    if (!blob) {
      return std::nullopt;
    }
    WireReader reader(*blob);
    const auto name = reader.string();
    if (!name || !isAscii(*name)) {
      return std::nullopt;
    }
    const auto type = skKeyType(asText(*name));
    if (!type || *type != *declared) {
      return std::nullopt;
    }
    if (*type == "ecdsa-sk" && !reader.string()) { // the curve
      return std::nullopt;
    }
    if (!reader.string()) { // the point
      return std::nullopt;
    }
    const auto application = reader.string();
    if (!application || !reader.atEnd() || !isAscii(*application)) {
      return std::nullopt;
    }
    return SkPublic{.type = std::string(*type), .application = asText(*application), .blob = std::move(*blob)};
  }

  std::string sshFingerprint(const Bytes& blob) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), blob.data(), blob.size());
    std::string encoded = Base64::encode(digest.data(), digest.size());
    while (!encoded.empty() && encoded.back() == '=') {
      encoded.pop_back();
    }
    return "SHA256:" + encoded;
  }

  std::optional<SkHandle> parseSkHandle(std::string_view text) {
    constexpr std::string_view kBegin = "-----BEGIN OPENSSH PRIVATE KEY-----";
    constexpr std::string_view kEnd = "-----END OPENSSH PRIVATE KEY-----";
    const auto begin = text.find(kBegin);
    if (begin == std::string_view::npos) {
      return std::nullopt;
    }
    const auto bodyStart = begin + kBegin.size();
    const auto end = text.find(kEnd, bodyStart);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    std::string body;
    for (const auto& part : splitWhitespace(text.substr(bodyStart, end - bodyStart))) {
      body += part;
    }
    const auto data = decodeBase64Strict(body);
    constexpr std::string_view kMagic("openssh-key-v1\0", 15);
    if (!data || data->size() < kMagic.size() || !std::equal(kMagic.begin(), kMagic.end(), data->begin())) {
      return std::nullopt;
    }
    WireReader reader(*data, kMagic.size());
    const auto cipher = reader.string();
    if (!cipher || !reader.string() || !reader.string()) { // the KDF name and options
      return std::nullopt;
    }
    const std::size_t at = reader.position();
    if (at + 4 > data->size()
        || (*data)[at] != 0
        || (*data)[at + 1] != 0
        || (*data)[at + 2] != 0
        || (*data)[at + 3] != 1) {
      return std::nullopt; // exactly one key
    }
    (void)reader.skip(4);
    auto blob = reader.string();
    const auto priv = reader.string();
    if (!blob || !priv) {
      return std::nullopt;
    }
    if (asText(*cipher) != "none") {
      return SkHandle{.blob = std::move(*blob), .encrypted = true, .credential = {}};
    }
    // Two check integers, then the key: its type, the public part, the application, the flags
    // byte and the key handle (the credential ID).
    if (priv->size() < 8 || !std::equal(priv->begin(), priv->begin() + 4, priv->begin() + 4)) {
      return std::nullopt;
    }
    WireReader key(*priv, 8);
    const auto name = key.string();
    if (!name || !isAscii(*name)) {
      return std::nullopt;
    }
    const auto type = skKeyType(asText(*name));
    if (!type) {
      return std::nullopt;
    }
    if (*type == "ecdsa-sk" && !key.string()) {
      return std::nullopt;
    }
    if (!key.string() || !key.string() || !key.skip(1)) { // point, application, flags
      return std::nullopt;
    }
    auto credential = key.string();
    if (!credential || credential->empty()) {
      return std::nullopt;
    }
    return SkHandle{.blob = std::move(*blob), .encrypted = false, .credential = std::move(*credential)};
  }

  std::vector<TokenCredential> parseTokenCredentials(std::string_view text) {
    static const std::regex kCredentialId("[A-Za-z0-9+/]{16,1024}={0,2}");
    std::vector<TokenCredential> found;
    for (const auto& line : splitLines(text)) {
      const auto fields = splitWhitespace(line);
      if (fields.size() < 5 || !fields[0].ends_with(':') || !std::regex_match(fields[1], kCredentialId)) {
        continue;
      }
      if (auto raw = decodeBase64Strict(fields[1])) {
        found.push_back({.id = fields[1], .raw = std::move(*raw)});
      }
    }
    return found;
  }

  bool isResidentKeyName(std::string_view name) {
    static const std::regex kResident("^id_\\w+_sk_rk");
    return !name.ends_with(".pub") && std::regex_search(name.begin(), name.end(), kResident);
  }

  std::string residentKeyName(std::string_view algorithm, std::string_view application, std::string_view user) {
    std::string suffix(application);
    if (!user.empty()) {
      suffix += suffix.empty() ? std::string(user) : "_" + std::string(user);
    }
    return "id_" + std::string(algorithmFilePart(algorithm)) + "_rk" + (suffix.empty() ? "" : "_" + suffix);
  }

  std::string residentKeyUser(std::string_view name, std::string_view keyType, std::string_view application) {
    const auto part = algorithmFilePart(keyType);
    if (part.empty()) {
      return {};
    }
    const std::string prefix = "id_" + std::string(part) + "_rk";
    if (!name.starts_with(prefix)) {
      return {};
    }
    std::string_view rest = name.substr(prefix.size());
    const std::string_view app = application.starts_with("ssh:") ? application.substr(4) : application;
    if (!app.empty()) {
      if (!rest.starts_with('_') || !rest.substr(1).starts_with(app)) {
        return {};
      }
      rest = rest.substr(app.size() + 1);
    }
    return rest.starts_with('_') ? std::string(rest.substr(1)) : std::string{};
  }

  std::string sshCreateProblem(const SshCreateOptions& options) {
    static const std::regex kApplication("[A-Za-z0-9](?:[A-Za-z0-9._@:-]{0,126}[A-Za-z0-9])?");
    static const std::regex kUser("[A-Za-z0-9](?:[A-Za-z0-9._@-]{0,62}[A-Za-z0-9])?");
    if (algorithmFilePart(options.algorithm).empty()) {
      return "algorithm";
    }
    if (!options.application.empty()
        && (!std::regex_match(options.application, kApplication) || options.application.contains(".."))) {
      return "application";
    }
    if (!options.user.empty() && (!std::regex_match(options.user, kUser) || options.user.contains(".."))) {
      return "user";
    }
    // ssh-keygen -K would skip a handle named like a public half.
    if (residentKeyName(options.algorithm, options.application, options.user).ends_with(".pub")) {
      return "pub-name";
    }
    // The .pub file carries the comment; keep it plain ASCII.
    if (options.comment.size() > 128
        || !std::ranges::all_of(options.comment, [](char c) { return c >= ' ' && c <= '~'; })) {
      return "comment";
    }
    return {};
  }

  std::vector<std::string> sshCreateCommand(const std::string& path, const SshCreateOptions& options) {
    std::vector<std::string> command{
        "ssh-keygen", "-t", options.algorithm, "-O", "resident", "-O", "application=ssh:" + options.application,
    };
    if (!options.user.empty()) {
      command.insert(command.end(), {"-O", "user=" + options.user});
    }
    if (options.verify) {
      command.insert(command.end(), {"-O", "verify-required"});
    }
    if (!options.touch) {
      command.insert(command.end(), {"-O", "no-touch-required"});
    }
    command.insert(command.end(), {"-C", options.comment, "-N", "", "-f", path});
    return command;
  }

  std::optional<GitRemote> gitRemote(std::string_view application) {
    static const std::regex kGitApplication(
        "(?://)?(?:([A-Za-z0-9][A-Za-z0-9._-]*)@)?"
        "([A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?(?:\\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+)"
        "(?::([0-9]{1,5}))?/?"
    );
    if (!application.starts_with("ssh:")) {
      return std::nullopt;
    }
    const std::string rest(application.substr(4));
    std::smatch match;
    if (!std::regex_match(rest, match, kGitApplication)) {
      return std::nullopt;
    }
    std::string host = match[2].str();
    std::ranges::transform(host, host.begin(), [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    std::string port;
    if (match[3].matched) {
      const auto value = parseInt(match[3].str());
      if (!value || *value < 1 || *value > 65535) {
        return std::nullopt;
      }
      port = std::to_string(*value);
    }
    bool known = std::ranges::contains(kGitHosts, std::string_view(host));
    if (!known) {
      const auto labels = StringUtils::split(host, '.');
      known =
          std::any_of(labels.begin(), labels.end() - 1, [](std::string_view label) { return label.contains("git"); });
    }
    if (!known) {
      return std::nullopt;
    }
    return GitRemote{.login = match[1].matched ? match[1].str() : "git", .host = std::move(host), .port = port};
  }

  std::string dropInText(const std::vector<std::string>& names, const GitRemote& remote, std::optional<int> minutes) {
    return dropInWith(kDropInMarker, names, remote, minutes);
  }

  bool validPersistMinutes(int minutes) { return minutes >= 0 && minutes <= kMaxPersistMinutes; }

  DropIn parseDropIn(std::string_view text) {
    const auto lines = allLines(text);
    if (lines.empty() || lines.front() != kDropInMarker) {
      return {.kind = DropInKind::Foreign};
    }
    static const std::regex kMinutes("([1-9][0-9]{0,3})m");
    DropIn found{.kind = DropInKind::Ours};
    bool invalidMinutes = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
      const std::string_view line = lines[i];
      if (line.starts_with("Host ")) {
        found.remote.host = line.substr(5);
      } else if (line.starts_with("    User ")) {
        found.remote.login = line.substr(9);
      } else if (line.starts_with("    Port ")) {
        found.remote.port = line.substr(9);
      } else if (line.starts_with(kIdentity)) {
        found.names.emplace_back(line.substr(kIdentity.size()));
      } else if (line.starts_with("    ControlPersist ")) {
        const std::string value(line.substr(19));
        std::smatch match;
        if (value == "no") {
          found.minutes = 0;
        } else if (std::regex_match(value, match, kMinutes)) {
          found.minutes = parseInt(match[1].str());
        } else {
          invalidMinutes = true;
        }
      }
    }
    if (found.remote.host.empty()
        || found.remote.login.empty()
        || invalidMinutes
        || (found.minutes.has_value() && !validPersistMinutes(*found.minutes))
        || dropInWith(lines.front(), found.names, found.remote, found.minutes) != text) {
      found.kind = DropInKind::Edited;
      found.minutes.reset();
    }
    return found;
  }

  bool includesDropIns(std::string_view config) {
    static const std::regex kInclude(R"(^\s*include\s+\S*config\.d/)", std::regex::icase);
    return std::ranges::any_of(allLines(config), [](std::string_view line) {
      return std::regex_search(line.begin(), line.end(), kInclude);
    });
  }

} // namespace security_hw
