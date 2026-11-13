#include "shell/dialogs/file_dialog.h"
#include "shell/settings/path_browse.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-path-browse-" + std::to_string(::getpid()));
  const auto directory = root / "notes";
  const auto file = directory / "config.toml";
  std::filesystem::create_directories(directory);
  std::ofstream{file} << "key = 'value'\n";

  FileDialogOptions folderOptions;
  applyPathDialogStartValue(folderOptions, directory.string(), PathBrowseKind::Folder);

  FileDialogOptions fileOptions;
  applyPathDialogStartValue(fileOptions, file.string(), PathBrowseKind::File);

  FileDialogOptions missingChildOptions;
  applyPathDialogStartValue(missingChildOptions, (directory / "new.toml").string(), PathBrowseKind::File);

  FileDialogOptions missingOptions;
  applyPathDialogStartValue(missingOptions, (root / "missing" / "file.toml").string(), PathBrowseKind::File);

  std::filesystem::remove_all(root);

  TEST_CHECK(folderOptions.startDirectory == directory);
  TEST_CHECK(fileOptions.startDirectory == directory);
  TEST_CHECK(fileOptions.defaultFilename == "config.toml");
  TEST_CHECK(missingChildOptions.startDirectory == directory);
  TEST_CHECK(missingOptions.startDirectory.empty());
  return 0;
}
