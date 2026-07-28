#include "shell/settings/path_browse.h"
#include "ui/dialogs/file_dialog.h"

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace {

  int g_failures = 0;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "path_browse_test: FAIL: {}", message);
      ++g_failures;
    }
  }

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-path-browse-" + std::to_string(::getpid()));
  const auto directory = root / "notes";
  const auto file = directory / "config.toml";
  std::filesystem::create_directories(directory);
  std::ofstream{file} << "key = 'value'\n";

  FileDialogOptions folderOptions;
  applyPathDialogStartValue(folderOptions, directory.string(), PathBrowseKind::Folder);
  expect(folderOptions.startDirectory == directory, "folder paths should open the selected directory");

  FileDialogOptions fileOptions;
  applyPathDialogStartValue(fileOptions, file.string(), PathBrowseKind::File);
  expect(fileOptions.startDirectory == directory, "file paths should open their parent directory");
  expect(fileOptions.defaultFilename == "config.toml", "file paths should preselect their filename");

  FileDialogOptions missingChildOptions;
  applyPathDialogStartValue(missingChildOptions, (directory / "new.toml").string(), PathBrowseKind::File);
  expect(missingChildOptions.startDirectory == directory, "missing paths should use an existing parent directory");

  FileDialogOptions missingOptions;
  applyPathDialogStartValue(missingOptions, (root / "missing" / "file.toml").string(), PathBrowseKind::File);
  expect(missingOptions.startDirectory.empty(), "paths without an existing parent should not change the dialog start");

  std::filesystem::remove_all(root);
  return g_failures == 0 ? 0 : 1;
}
