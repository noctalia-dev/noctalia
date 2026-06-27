#include "system/distro_info.h"

#include "i18n/i18n.h"
#include "util/string_utils.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {

  std::optional<std::unordered_map<std::string, std::string>> parseOsRelease(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      return std::nullopt;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(file, line)) {
      const auto trimmed = StringUtils::trim(line);
      if (trimmed.empty() || trimmed.front() == '#') {
        continue;
      }

      const auto eq = trimmed.find('=');
      if (eq == std::string::npos || eq == 0) {
        continue;
      }

      auto key = std::string(trimmed.substr(0, eq));
      auto value = StringUtils::unquote(StringUtils::trim(std::string_view(trimmed).substr(eq + 1)));
      values[std::move(key)] = std::move(value);
    }

    return values;
  }

} // namespace

std::optional<DistroInfo> DistroDetector::detect() {
  const std::filesystem::path candidates[] = {"/etc/os-release", "/usr/lib/os-release"};

  for (const auto& path : candidates) {
    const auto parsed = parseOsRelease(path);
    if (!parsed.has_value()) {
      continue;
    }

    DistroInfo info;
    if (const auto it = parsed->find("ID"); it != parsed->end()) {
      info.id = it->second;
    }
    if (const auto it = parsed->find("NAME"); it != parsed->end()) {
      info.name = it->second;
    }
    if (const auto it = parsed->find("VERSION"); it != parsed->end()) {
      info.version = it->second;
    }
    if (const auto it = parsed->find("PRETTY_NAME"); it != parsed->end()) {
      info.prettyName = it->second;
    }

    if (!info.prettyName.empty() || !info.name.empty() || !info.id.empty()) {
      return info;
    }
  }

  return std::nullopt;
}

std::string distroLabel() {
  if (const auto distro = DistroDetector::detect(); distro.has_value()) {
    if (!distro->prettyName.empty()) {
      return distro->prettyName;
    }
    if (!distro->name.empty()) {
      return distro->name;
    }
    if (!distro->id.empty()) {
      return distro->id;
    }
  }
  return i18n::tr("system.hardware.unknown-distro");
}

std::string kernelLabel() {
  struct utsname un{};
  if (uname(&un) == 0 && un.release[0] != '\0') {
    if (un.sysname[0] != '\0') {
      return std::string(un.sysname) + " " + un.release;
    }
    return un.release;
  }
  return i18n::tr("control-center.system.unknown");
}

std::string osAgeLabel() {
  std::uint64_t oldest = 0;

  for (const char* path : {"/", "/etc", "/var", "/home"}) {
    struct statx sx{};
    if (statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) == 0
        && (sx.stx_mask & STATX_BTIME) != 0
        && sx.stx_btime.tv_sec > 0) {
      const auto birth = static_cast<std::uint64_t>(sx.stx_btime.tv_sec);
      if (oldest == 0 || birth < oldest) {
        oldest = birth;
      }
    }
  }

  if (oldest == 0) {
    struct stat st{};
    if (stat("/etc/machine-id", &st) == 0 && st.st_mtime > 0) {
      oldest = static_cast<std::uint64_t>(st.st_mtime);
    }
  }

  if (oldest == 0) {
    return i18n::tr("control-center.system.unknown");
  }
  const std::time_t now = std::time(nullptr);
  if (now <= 0 || static_cast<std::uint64_t>(now) <= oldest) {
    return i18n::tr("time.duration.less-than-day");
  }

  const std::uint64_t seconds = static_cast<std::uint64_t>(now) - oldest;
  const std::uint64_t days = seconds / 86400;
  const std::uint64_t years = days / 365;
  const std::uint64_t months = (days % 365) / 30;
  if (years > 0) {
    const std::string yearText = i18n::trp("time.units.year", static_cast<long>(years));
    if (months > 0) {
      const std::string monthText = i18n::trp("time.units.month", static_cast<long>(months));
      return i18n::tr("time.duration.two-parts", "first", yearText, "second", monthText);
    }
    return yearText;
  }
  return i18n::trp("time.units.day", static_cast<long>(days));
}

std::string sessionDisplayName() {
  struct passwd* pw = getpwuid(getuid());
  const char* loginEnv = std::getenv("USER");
  std::string login = "user";
  if (pw != nullptr) {
    login = pw->pw_name;
  } else if (loginEnv != nullptr) {
    login = loginEnv;
  }

  if (pw != nullptr && pw->pw_gecos != nullptr && pw->pw_gecos[0] != '\0') {
    std::string gecos = pw->pw_gecos;
    const auto comma = gecos.find(',');
    return comma == std::string::npos ? gecos : gecos.substr(0, comma);
  }
  return login;
}

std::string hostName() {
  struct utsname un{};
  if (uname(&un) == 0 && un.nodename[0] != '\0') {
    return un.nodename;
  }
  return i18n::tr("control-center.system.unknown");
}

std::optional<std::chrono::seconds> systemUptime() {
  std::ifstream in{"/proc/uptime"};
  double up = 0.0;
  double idleDummy = 0.0;
  if (in >> up >> idleDummy) {
    return std::chrono::seconds{static_cast<std::int64_t>(up)};
  }
  return std::nullopt;
}
