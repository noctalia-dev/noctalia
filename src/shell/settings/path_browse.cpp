#include "shell/settings/path_browse.h"

#include "ui/dialogs/file_dialog.h"

#include <filesystem>

void applyPathDialogStartValue(FileDialogOptions& options, std::string_view currentValue, PathBrowseKind kind) {
  if (currentValue.empty()) {
    return;
  }

  const std::filesystem::path current(currentValue);
  std::error_code ec;
  if (kind == PathBrowseKind::Folder
      && std::filesystem::exists(current, ec)
      && std::filesystem::is_directory(current, ec)) {
    options.startDirectory = current;
    return;
  }
  if (kind == PathBrowseKind::File
      && std::filesystem::exists(current, ec)
      && std::filesystem::is_regular_file(current, ec)) {
    options.startDirectory = current.parent_path();
    options.defaultFilename = current.filename().string();
    return;
  }
  if (current.has_parent_path()
      && std::filesystem::exists(current.parent_path(), ec)
      && std::filesystem::is_directory(current.parent_path(), ec)) {
    options.startDirectory = current.parent_path();
  }
}
