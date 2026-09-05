#include "system/process_service.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

namespace {
  constexpr Logger kLog("process");

  [[nodiscard]] std::uint64_t readTotalCpuTicks() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return 0;
    std::string line;
    std::getline(file, line);
    if (!line.starts_with("cpu ")) return 0;
    std::istringstream iss(line.substr(4));
    std::uint64_t sum = 0, v = 0;
    while (iss >> v) sum += v;
    return sum;
  }

  [[nodiscard]] std::string userForUid(uid_t uid) {
    if (auto* pw = getpwuid(uid)) return pw->pw_name;
    return std::to_string(uid);
  }

  [[nodiscard]] bool parseProcStat(pid_t pid, ProcessInfo& out) {
    const std::string path = std::format("/proc/{}/stat", pid);
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string content;
    std::getline(file, content);
    if (content.empty()) return false;
    // comm is between '(' and ')'
    auto l = content.find('(');
    auto r = content.rfind(')');
    if (l == std::string::npos || r == std::string::npos || r <= l) return false;
    out.name = content.substr(l + 1, r - l - 1);
    // rest after ')'
    std::string rest = content.substr(r + 1);
    std::istringstream iss(rest);
    char state = 0;
    iss >> state;
    out.state = state;
    // fields: ppid(1) pgrp(2) session(3) tty(4) tpgid(5) flags(6) minflt(7) cminflt(8) majflt(9) cmajflt(10)
    // utime(11) stime(12) cutime(13) cstime(14) priority(15) nice(16) num_threads(17) itreal(18) starttime(19)
    // vsize(20) rss(21)
    long long dummy = 0;
    unsigned long long utime = 0, stime = 0;
    // skip to utime
    for (int i = 1; i <= 10; ++i) { if (!(iss >> dummy)) return false; }
    if (!(iss >> utime)) return false;
    if (!(iss >> stime)) return false;
    // skip cutime, cstime, priority, nice, num_threads, itreal, starttime, vsize
    for (int i = 0; i < 8; ++i) { long long v = 0; iss >> v; }
    long rssPages = 0;
    iss >> rssPages;
    out.cpuTicks = utime + stime;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;
    std::uint64_t rssBytes = static_cast<std::uint64_t>(rssPages) * static_cast<std::uint64_t>(pageSize);
    out.rssKb = rssBytes / 1024;
    return true;
  }

  [[nodiscard]] bool readUid(pid_t pid, uid_t& uidOut) {
    std::ifstream file(std::format("/proc/{}/status", pid));
    if (!file.is_open()) return false;
    std::string line;
    while (std::getline(file, line)) {
      if (line.starts_with("Uid:")) {
        std::istringstream iss(line.substr(4));
        uid_t real = 0, eff = 0, saved = 0, fs = 0;
        iss >> real >> eff >> saved >> fs;
        uidOut = real;
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool pidExists(pid_t pid) { return std::filesystem::exists(std::format("/proc/{}", pid)); }

} // namespace

std::vector<ProcessInfo> ProcessService::fetchProcesses() {
  std::vector<ProcessInfo> result;
  result.reserve(300);

  const std::uint64_t totalTicks = readTotalCpuTicks();
  const std::uint64_t totalDelta = (m_prevTotalTicks == 0 || totalTicks < m_prevTotalTicks) ? 0 : (totalTicks - m_prevTotalTicks);
  const bool haveDelta = !m_firstSample && totalDelta > 0;
  const long nCpus = sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1;

  std::unordered_map<pid_t, std::uint64_t> newTicks;
  newTicks.reserve(512);

  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec) break;
    if (!entry.is_directory(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
    pid_t pid = static_cast<pid_t>(std::stoi(name));
    ProcessInfo info;
    info.pid = pid;
    if (!parseProcStat(pid, info)) continue;
    uid_t uid = 0;
    if (!readUid(pid, uid)) continue;
    info.uid = uid;
    info.user = userForUid(uid);
    // CPU percent via delta
    auto it = m_prevTicks.find(pid);
    if (haveDelta && it != m_prevTicks.end()) {
      const std::uint64_t delta = (info.cpuTicks >= it->second) ? (info.cpuTicks - it->second) : 0;
      info.cpuPercent = (static_cast<double>(delta) / static_cast<double>(totalDelta)) * 100.0 * static_cast<double>(nCpus);
      if (info.cpuPercent < 0) info.cpuPercent = 0;
      if (info.cpuPercent > 100.0 * nCpus) info.cpuPercent = 100.0 * nCpus;
    } else {
      info.cpuPercent = 0.0;
    }
    newTicks[pid] = info.cpuTicks;
    result.push_back(std::move(info));
  }

  m_prevTicks = std::move(newTicks);
  m_prevTotalTicks = totalTicks;
  m_firstSample = false;
  return result;
}

bool ProcessService::killProcess(pid_t pid, std::string& error) {
  if (pid <= 1 || pid == getpid()) {
    error = "Refusing to kill pid " + std::to_string(pid);
    return false;
  }
  if (!pidExists(pid)) {
    error = "Process not found";
    return false;
  }
  // user-only: check uid
  uid_t uid = 0;
  if (readUid(pid, uid)) {
    if (uid != static_cast<uid_t>(getuid())) {
      error = "Not your process (user-only)";
      return false;
    }
  }
  if (::kill(pid, SIGTERM) != 0) {
    error = std::string("SIGTERM failed: ") + std::strerror(errno);
    return false;
  }
  // Poll briefly for exit, then SIGKILL
  for (int i = 0; i < 10; ++i) {
    usleep(100000); // 100ms
    if (!pidExists(pid)) return true;
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
  }
  if (pidExists(pid)) {
    if (::kill(pid, SIGKILL) != 0) {
      if (errno == ESRCH) return true;
      error = std::string("SIGKILL failed: ") + std::strerror(errno);
      return false;
    }
    usleep(200000);
    if (!pidExists(pid)) return true;
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
  }
  return true;
}
