#pragma once

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace SysUtils {

  [[nodiscard]] inline bool isEnvFlagOn(const char* name) {
    const char* s = std::getenv(name);
    if (s == nullptr) {
      return false;
    }
    std::string_view sv(s);
    return !sv.empty() && sv != "0" && sv != "false" && sv != "no" && sv != "off";
  }

  // True for "cpuN" names (cpu0, cpu10, ...); rejects the aggregate "cpu" and anything else.
  [[nodiscard]] inline bool isCpuN(std::string_view name) {
    return name.size() > 3
        && name.starts_with("cpu")
        && std::ranges::all_of(name.substr(3), [](char c) { return c >= '0' && c <= '9'; });
  }

} // namespace SysUtils
