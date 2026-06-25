#include "system/easyeffects_service.h"

#include "core/log.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>

namespace {

  bool isProtocolSafePresetName(std::string_view name) {
    return !name.contains(':') && !name.contains('\r') && !name.contains('\n');
  }

  void pushUniqueProfile(std::vector<std::string>& profiles, std::unordered_set<std::string>& seen, std::string name) {
    name = StringUtils::trim(name);
    if (name.empty() || !isProtocolSafePresetName(name) || !seen.insert(name).second) {
      return;
    }
    profiles.push_back(std::move(name));
  }

  std::string presetNameFromUsageEntry(std::string_view entry) {
    const auto colon = entry.find(':');
    if (colon != std::string_view::npos) {
      entry = entry.substr(0, colon);
    }
    return StringUtils::trim(entry);
  }

  void appendCommaSeparatedPresetNames(
      std::vector<std::string>& profiles, std::unordered_set<std::string>& seen, std::string_view value,
      bool stripUsageCount
  ) {
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t end = value.find(',', start);
      std::string_view item(value.data() + start, (end == std::string_view::npos ? value.size() : end) - start);
      std::string name = stripUsageCount ? presetNameFromUsageEntry(item) : StringUtils::trim(item);
      pushUniqueProfile(profiles, seen, std::move(name));
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
  }

  struct EasyEffectsPresetConfig {
    std::vector<std::string> outputProfiles;
    std::vector<std::string> inputProfiles;
    std::string lastLoadedOutputPreset;
    std::string lastLoadedInputPreset;
  };

  EasyEffectsPresetConfig readEasyEffectsPresetConfig(const std::filesystem::path& file) {
    EasyEffectsPresetConfig result;
    std::unordered_set<std::string> seenOutputs;
    std::unordered_set<std::string> seenInputs;
    std::ifstream in(file);
    if (!in) {
      return result;
    }

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
      line = StringUtils::trim(line);
      if (line.empty() || line.front() == '#' || line.front() == ';') {
        continue;
      }
      if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
        section = line.substr(1, line.size() - 2);
        continue;
      }

      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = StringUtils::trim(std::string_view(line.data(), eq));
      const std::string value = StringUtils::trim(std::string_view(line.data() + eq + 1, line.size() - eq - 1));

      if (key == "lastLoadedOutputPreset") {
        result.lastLoadedOutputPreset = value;
        pushUniqueProfile(result.outputProfiles, seenOutputs, value);
      } else if (key == "lastLoadedInputPreset") {
        result.lastLoadedInputPreset = value;
        pushUniqueProfile(result.inputProfiles, seenInputs, value);
      } else if (section == "StreamOutputs") {
        if (key == "usedPresets") {
          appendCommaSeparatedPresetNames(result.outputProfiles, seenOutputs, value, true);
        } else if (key == "mostUsedPresets") {
          appendCommaSeparatedPresetNames(result.outputProfiles, seenOutputs, value, false);
        }
      } else if (section == "StreamInputs") {
        if (key == "usedPresets") {
          appendCommaSeparatedPresetNames(result.inputProfiles, seenInputs, value, true);
        } else if (key == "mostUsedPresets") {
          appendCommaSeparatedPresetNames(result.inputProfiles, seenInputs, value, false);
        }
      }
    }

    return result;
  }

  void appendPresetFilesFromDir(
      std::vector<std::string>& profiles, std::unordered_set<std::string>& seen, const std::filesystem::path& dir
  ) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
      return;
    }

    for (const auto& entry :
         std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (!entry.is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      const auto& path = entry.path();
      if (path.extension() != ".json") {
        continue;
      }
      pushUniqueProfile(profiles, seen, path.stem().string());
    }
  }

  std::filesystem::path homePath() {
    const char* home = std::getenv("HOME");
    return home != nullptr && home[0] != '\0' ? std::filesystem::path(home) : std::filesystem::path{};
  }

  void
  appendColonSeparatedRoots(std::vector<std::filesystem::path>& roots, std::string_view dirs, std::string_view appDir) {
    std::size_t start = 0;
    while (start <= dirs.size()) {
      const std::size_t end = dirs.find(':', start);
      const std::string_view item(dirs.data() + start, (end == std::string::npos ? dirs.size() : end) - start);
      if (!item.empty()) {
        roots.emplace_back(std::filesystem::path(item) / appDir);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }

  std::vector<std::filesystem::path> easyeffectsConfigRoots() {
    std::vector<std::filesystem::path> roots;

    const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
    if (xdgConfigHome != nullptr && xdgConfigHome[0] != '\0') {
      roots.emplace_back(std::filesystem::path(xdgConfigHome) / "easyeffects");
    } else if (const auto home = homePath(); !home.empty()) {
      roots.emplace_back(home / ".config/easyeffects");
    }

    const char* xdgConfigDirs = std::getenv("XDG_CONFIG_DIRS");
    const std::string dirs =
        (xdgConfigDirs != nullptr && xdgConfigDirs[0] != '\0') ? xdgConfigDirs : std::string{"/etc/xdg"};
    appendColonSeparatedRoots(roots, dirs, "easyeffects");

    if (const auto home = homePath(); !home.empty()) {
      roots.emplace_back(home / ".var/app/com.github.wwmm.easyeffects/config/easyeffects");
    }

    return roots;
  }

  std::vector<std::filesystem::path> easyeffectsDataRoots() {
    std::vector<std::filesystem::path> roots;

    const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
    if (xdgDataHome != nullptr && xdgDataHome[0] != '\0') {
      roots.emplace_back(std::filesystem::path(xdgDataHome) / "easyeffects");
    } else if (const auto home = homePath(); !home.empty()) {
      roots.emplace_back(home / ".local/share/easyeffects");
    }

    const char* xdgDataDirs = std::getenv("XDG_DATA_DIRS");
    const std::string dirs =
        (xdgDataDirs != nullptr && xdgDataDirs[0] != '\0') ? xdgDataDirs : std::string{"/usr/local/share:/usr/share"};
    appendColonSeparatedRoots(roots, dirs, "easyeffects");

    if (const auto home = homePath(); !home.empty()) {
      roots.emplace_back(home / ".var/app/com.github.wwmm.easyeffects/data/easyeffects");
    }

    return roots;
  }

  std::optional<AudioEffectsProfileKind> parseEffectsProfileKind(std::string_view value) {
    const std::string trimmed = StringUtils::trim(value);
    if (trimmed == "output" || trimmed == "out" || trimmed == "sink") {
      return AudioEffectsProfileKind::Output;
    }
    if (trimmed == "input" || trimmed == "in" || trimmed == "source" || trimmed == "mic") {
      return AudioEffectsProfileKind::Input;
    }
    return std::nullopt;
  }

  std::string effectsProfileKindName(AudioEffectsProfileKind kind) {
    return kind == AudioEffectsProfileKind::Input ? "input" : "output";
  }

  std::string availableEffectsProfilesSuffix(const std::vector<std::string>& profiles) {
    if (profiles.empty()) {
      return "\n";
    }
    std::string suffix = "; available:";
    for (const auto& profile : profiles) {
      suffix += std::format(" {}", profile);
    }
    suffix.push_back('\n');
    return suffix;
  }

  std::filesystem::path easyEffectsServerPath() {
    if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir != nullptr && runtimeDir[0] != '\0') {
      return std::filesystem::path(runtimeDir) / "EasyEffectsServer";
    }
    return std::filesystem::path("/run/user") / std::to_string(::getuid()) / "EasyEffectsServer";
  }

  bool easyeffectsRunning() {
    std::error_code ec;
    return std::filesystem::exists(easyEffectsServerPath(), ec) && !ec;
  }

  std::vector<std::string> discoverEffectsProfiles(AudioEffectsProfileKind kind) {
    std::vector<std::string> profiles;
    std::unordered_set<std::string> seen;

    if (!easyeffectsRunning()) {
      return profiles;
    }

    const auto profileDir = kind == AudioEffectsProfileKind::Input ? "input" : "output";
    for (const auto& root : easyeffectsDataRoots()) {
      appendPresetFilesFromDir(profiles, seen, root / profileDir);
    }

    const bool foundDataProfiles = !profiles.empty();
    for (const auto& root : easyeffectsConfigRoots()) {
      if (!foundDataProfiles) {
        appendPresetFilesFromDir(profiles, seen, root / profileDir);
        const auto config = readEasyEffectsPresetConfig(root / "db/easyeffectsrc");
        const auto& discovered = kind == AudioEffectsProfileKind::Input ? config.inputProfiles : config.outputProfiles;
        for (const auto& profile : discovered) {
          pushUniqueProfile(profiles, seen, profile);
        }
      }
    }

    std::ranges::sort(profiles, [](const std::string& a, const std::string& b) {
      return StringUtils::toLower(a) < StringUtils::toLower(b);
    });
    return profiles;
  }

  std::string discoverActiveEffectsProfile(AudioEffectsProfileKind kind) {
    if (!easyeffectsRunning()) {
      return {};
    }
    for (const auto& root : easyeffectsConfigRoots()) {
      const auto config = readEasyEffectsPresetConfig(root / "db/easyeffectsrc");
      if (kind == AudioEffectsProfileKind::Output && !config.lastLoadedOutputPreset.empty()) {
        return config.lastLoadedOutputPreset;
      }
      if (kind == AudioEffectsProfileKind::Input && !config.lastLoadedInputPreset.empty()) {
        return config.lastLoadedInputPreset;
      }
    }
    return {};
  }

  std::optional<std::string> exchangeEasyEffectsServerCommand(std::string_view command, bool readResponse) {
    const std::filesystem::path path = easyEffectsServerPath();
    const std::string nativePath = path.string();
    if (nativePath.empty() || nativePath.size() >= sizeof(sockaddr_un::sun_path)) {
      return std::nullopt;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return std::nullopt;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, nativePath.c_str(), sizeof(addr.sun_path) - 1);

    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      std::string payload(command);
      payload.push_back('\n');
      const char* data = payload.data();
      std::size_t remaining = payload.size();
      ok = true;
      while (remaining > 0) {
        const ssize_t n = ::send(fd, data, remaining, MSG_NOSIGNAL);
        if (n > 0) {
          const auto written = static_cast<std::size_t>(n);
          data += written;
          remaining -= written;
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        ok = false;
        break;
      }
    }

    std::string response;
    bool receivedResponse = false;
    if (ok && readResponse) {
      (void)::shutdown(fd, SHUT_WR);
      std::array<char, 512> buffer{};
      while (true) {
        pollfd pfd{.fd = fd, .events = POLLIN | POLLHUP, .revents = 0};
        const int pollRc = ::poll(&pfd, 1, 250);
        if (pollRc < 0 && errno == EINTR) {
          continue;
        }
        if (pollRc <= 0) {
          break;
        }

        const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (n > 0) {
          receivedResponse = true;
          response.append(buffer.data(), static_cast<std::size_t>(n));
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
    }

    ::close(fd);
    if (!ok) {
      return std::nullopt;
    }
    if (readResponse && !receivedResponse) {
      return std::nullopt;
    }
    return response;
  }

  bool sendEasyEffectsServerCommand(std::string_view command) {
    return exchangeEasyEffectsServerCommand(command, false).has_value();
  }

  std::string easyEffectsLoadPresetCommand(AudioEffectsProfileKind kind, std::string_view profile) {
    return std::format("load_preset:{}:{}", effectsProfileKindName(kind), profile);
  }

  std::string easyEffectsGetLastLoadedPresetCommand(AudioEffectsProfileKind kind) {
    return std::format("get_last_loaded_preset:{}", effectsProfileKindName(kind));
  }

  std::optional<std::string> queryEasyEffectsActivePreset(AudioEffectsProfileKind kind) {
    const auto response = exchangeEasyEffectsServerCommand(easyEffectsGetLastLoadedPresetCommand(kind), true);
    if (!response.has_value()) {
      return std::nullopt;
    }
    return StringUtils::trim(*response);
  }

} // namespace

const std::vector<std::string>& EasyEffectsService::effectsProfiles(AudioEffectsProfileKind kind) const {
  return kind == AudioEffectsProfileKind::Input ? m_inputEffectsProfiles : m_outputEffectsProfiles;
}

std::string EasyEffectsService::activeEffectsProfile(AudioEffectsProfileKind kind) const {
  const std::optional<std::string>& active =
      kind == AudioEffectsProfileKind::Input ? m_activeInputEffectsProfile : m_activeOutputEffectsProfile;
  return active.value_or(std::string{});
}

void EasyEffectsService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void EasyEffectsService::refreshProfiles() {
  const auto nextOutput = discoverEffectsProfiles(AudioEffectsProfileKind::Output);
  const auto nextInput = discoverEffectsProfiles(AudioEffectsProfileKind::Input);
  if (nextOutput == m_outputEffectsProfiles && nextInput == m_inputEffectsProfiles) {
    return;
  }
  m_outputEffectsProfiles = nextOutput;
  m_inputEffectsProfiles = nextInput;
  emitChanged();
}

void EasyEffectsService::refreshActiveEffectsProfiles() {
  bool changed = false;
  auto refreshKind = [this, &changed](AudioEffectsProfileKind kind) {
    auto active = queryEasyEffectsActivePreset(kind);
    if (!active.has_value()) {
      const std::string discovered = discoverActiveEffectsProfile(kind);
      if (discovered.empty()) {
        return;
      }
      active = discovered;
    }

    std::optional<std::string>& cached =
        kind == AudioEffectsProfileKind::Input ? m_activeInputEffectsProfile : m_activeOutputEffectsProfile;
    if (!cached.has_value() || *cached != *active) {
      cached = *active;
      changed = true;
    }
  };

  refreshKind(AudioEffectsProfileKind::Output);
  refreshKind(AudioEffectsProfileKind::Input);
  if (changed) {
    emitChanged();
  }
}

bool EasyEffectsService::loadEffectsProfile(AudioEffectsProfileKind kind, std::string_view profile) {
  static const Logger kLog("easyeffects");
  const std::string trimmed = StringUtils::trim(profile);
  if (trimmed.empty()) {
    return false;
  }
  if (!isProtocolSafePresetName(trimmed)) {
    kLog.warn(
        "refusing to load EasyEffects {} profile with protocol-unsafe name \"{}\"", effectsProfileKindName(kind),
        trimmed
    );
    return false;
  }

  if (!sendEasyEffectsServerCommand(easyEffectsLoadPresetCommand(kind, trimmed))) {
    kLog.warn(
        "failed to load EasyEffects {} profile \"{}\" - local server unavailable", effectsProfileKindName(kind), trimmed
    );
    return false;
  }

  const auto activePreset = queryEasyEffectsActivePreset(kind);
  if (!activePreset.has_value()) {
    kLog.warn(
        "failed to confirm EasyEffects {} profile \"{}\" after load command", effectsProfileKindName(kind), trimmed
    );
    return false;
  }
  if (*activePreset != trimmed) {
    kLog.warn(
        R"(EasyEffects {} profile load mismatch requested="{}" active="{}")", effectsProfileKindName(kind), trimmed,
        *activePreset
    );
    return false;
  }

  std::optional<std::string>& cached =
      kind == AudioEffectsProfileKind::Input ? m_activeInputEffectsProfile : m_activeOutputEffectsProfile;
  const bool changed = !cached.has_value() || *cached != *activePreset;
  cached = *activePreset;
  if (changed) {
    emitChanged();
  }
  return true;
}

void EasyEffectsService::registerIpc(
    IpcService& ipc, const ConfigService&, EffectsProfileFeedbackCallback effectsProfileFeedback
) {
  ipc.registerHandler(
      "effects-profile-set",
      [this, effectsProfileFeedback](const std::string& args) -> std::string {
        const auto trimmedView = std::string_view(args);
        const auto split = trimmedView.find(' ');
        if (split == std::string_view::npos) {
          return "error: effects-profile-set requires <output|input> <profile>\n";
        }
        const std::string kindArg = StringUtils::trim(trimmedView.substr(0, split));
        const auto kind = parseEffectsProfileKind(kindArg);
        if (!kind.has_value()) {
          return "error: effects-profile-set requires <output|input> <profile>\n";
        }
        const std::string profile = StringUtils::trim(trimmedView.substr(split));
        if (profile.empty()) {
          return "error: profile required\n";
        }

        refreshProfiles();
        const auto profiles = effectsProfiles(*kind);
        if (profiles.empty()) {
          return std::format("error: no EasyEffects {} profiles found\n", effectsProfileKindName(*kind));
        }
        if (!std::ranges::contains(profiles, profile)) {
          return std::format(
              "error: unknown EasyEffects {} profile \"{}\"{}", effectsProfileKindName(*kind), profile,
              availableEffectsProfilesSuffix(profiles)
          );
        }
        if (!loadEffectsProfile(*kind, profile)) {
          return std::format(
              "error: failed to set EasyEffects {} profile (is EasyEffects running?)\n", effectsProfileKindName(*kind)
          );
        }
        if (effectsProfileFeedback) {
          effectsProfileFeedback(*kind, profile);
        }
        return "ok\n";
      },
      "effects-profile-set <output|input> <profile>", "Set the EasyEffects output or input profile"
  );
}
