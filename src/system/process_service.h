#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
  pid_t pid = 0;
  std::string name; // comm
  uid_t uid = 0;
  std::string user;
  double cpuPercent = 0.0;
  std::uint64_t rssKb = 0;
  char state = '?';
  std::uint64_t cpuTicks = 0; // internal
};

enum class ProcessSort { Memory, Cpu };

class ProcessService {
public:
  ProcessService() = default;

  [[nodiscard]] std::vector<ProcessInfo> fetchProcesses();
  [[nodiscard]] bool killProcess(pid_t pid, std::string& error);

private:
  std::unordered_map<pid_t, std::uint64_t> m_prevTicks;
  std::uint64_t m_prevTotalTicks = 0;
  bool m_firstSample = true;
};
