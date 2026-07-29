#pragma once

#include <cstdint>
#include <string_view>

struct FileDialogOptions;

enum class PathBrowseKind : std::uint8_t {
  File,
  Folder,
};

// Seeds a file dialog from an existing path when that path or its parent exists.
void applyPathDialogStartValue(FileDialogOptions& options, std::string_view currentValue, PathBrowseKind kind);
