#pragma once

#include <filesystem>
#include <string>

namespace noctalia::util {

  // Lowercase hex MD5 of a file's contents. Empty string if the file cannot be read.
  std::string fileMd5Hex(const std::filesystem::path& path);

} // namespace noctalia::util
