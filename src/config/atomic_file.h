#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

struct AtomicWriteTarget {
  std::filesystem::path path;
  bool throughSymlink = false;
};

[[nodiscard]] std::optional<AtomicWriteTarget> resolveAtomicWriteTarget(const std::filesystem::path& path);
[[nodiscard]] bool writeTextFileAtomic(
    const std::filesystem::path& path, std::string_view content,
    std::optional<std::filesystem::perms> mode = std::nullopt
);
