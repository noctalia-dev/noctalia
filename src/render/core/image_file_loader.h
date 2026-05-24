#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct LoadedImageFile {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
};

[[nodiscard]] std::optional<LoadedImageFile>
loadImageFile(const std::string& path, int targetSize = 0, std::string* errorMessage = nullptr);
