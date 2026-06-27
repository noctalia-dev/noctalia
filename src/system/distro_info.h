#pragma once

#include <chrono>
#include <optional>
#include <string>

struct DistroInfo {
  std::string id;
  std::string name;
  std::string version;
  std::string prettyName;
};

class DistroDetector {
public:
  [[nodiscard]] static std::optional<DistroInfo> detect();
};

// Best-effort pretty label for the running distribution.
[[nodiscard]] std::string distroLabel();

// Kernel label: sysname + release (e.g. "Linux 6.9.3-..."), or "unknown" on failure.
[[nodiscard]] std::string kernelLabel();

// Approximate OS install age, formatted as "{y}y {m}mo" / "{y}y" / "{d}d".
[[nodiscard]] std::string osAgeLabel();

// Display name of the current session user (gecos or login).
[[nodiscard]] std::string sessionDisplayName();

// Machine hostname (uname nodename), or "unknown" on failure.
[[nodiscard]] std::string hostName();

// Returns system uptime (from /proc/uptime), or nullopt if unavailable.
[[nodiscard]] std::optional<std::chrono::seconds> systemUptime();
