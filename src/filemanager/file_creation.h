#pragma once

#include "filemanager/file_interaction.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace filemanager {
  inline std::optional<std::filesystem::path> creationDestinationWithExtension(
      const std::filesystem::path& directory, std::string name, std::string requiredExtension,
      std::error_code& error
  );

  // Resolve a user supplied name in the displayed directory without replacing
  // an existing entry. Text files receive a .txt suffix unless already present
  // (extension comparison is case-insensitive).
  inline std::optional<std::filesystem::path> creationDestination(
      const std::filesystem::path& directory, std::string name, bool textFile, std::error_code& error
  ) {
    error.clear();
    if (textFile)
      return creationDestinationWithExtension(directory, std::move(name), ".txt", error);
    return creationDestinationWithExtension(directory, std::move(name), {}, error);
  }

  inline std::optional<std::filesystem::path> creationDestinationWithExtension(
      const std::filesystem::path& directory, std::string name, std::string requiredExtension,
      std::error_code& error
  ) {
    error.clear();
    if (!requiredExtension.empty()) {
      const auto extension = std::filesystem::path(name).extension().string();
      std::string folded = extension;
      std::ranges::transform(folded, folded.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      std::ranges::transform(requiredExtension, requiredExtension.begin(),
                             [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (folded != requiredExtension)
        name += requiredExtension;
    }
    if (!validFileName(name)) {
      error = std::make_error_code(std::errc::invalid_argument);
      return std::nullopt;
    }

    const std::filesystem::path requested(name);
    const auto stem = requested.stem().string();
    const auto extension = requested.extension().string();
    for (unsigned suffix = 1; suffix < 10000; ++suffix) {
      const auto candidateName = suffix == 1 ? name : stem + " " + std::to_string(suffix) + extension;
      auto candidate = directory / candidateName;
      const bool occupied = std::filesystem::exists(candidate, error);
      if (error)
        return std::nullopt;
      if (!occupied)
        return candidate;
    }
    error = std::make_error_code(std::errc::file_exists);
    return std::nullopt;
  }
} // namespace filemanager
