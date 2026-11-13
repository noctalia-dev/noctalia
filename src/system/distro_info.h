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
  static std::optional<DistroInfo> detect();
};

// Best-effort pretty label for the running distribution.
std::string distroLabel();

// Kernel label: sysname + release (e.g. "Linux 6.9.3-..."), or "unknown" on failure.
std::string kernelLabel();

// Approximate OS install age, formatted as "{y}y {m}mo" / "{y}y" / "{d}d".
std::string osAgeLabel();

// Login name of the current session user, or its numeric UID if unavailable.
std::string sessionUserName();

// Display name of the current session user (gecos or login), or its numeric UID if unavailable.
std::string sessionDisplayName();

// Machine hostname (uname nodename), or "unknown" on failure.
std::string hostName();

// Returns system uptime (from /proc/uptime), or nullopt if unavailable.
std::optional<std::chrono::seconds> systemUptime();
