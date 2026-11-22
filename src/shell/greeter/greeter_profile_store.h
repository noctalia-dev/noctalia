#pragma once

#include "shell/greeter/greeter_profile.h"

#include <expected>
#include <filesystem>
#include <string>

namespace greeter {
  std::expected<std::filesystem::path, std::string> localProfilePath();
  // A missing file is an unmanaged profile; unreadable/invalid files report errors.
  std::expected<GreeterProfile, std::string> readLocalProfile(const std::filesystem::path& path);
  std::expected<void, std::string> writeLocalProfile(const std::filesystem::path& path, const GreeterProfile& profile);
} // namespace greeter
