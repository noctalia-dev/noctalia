#include "filemanager/startup_path.h"

#include <cassert>
#include <fstream>
#include <unistd.h>

int main() {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / ("nythera-startup-" + std::to_string(getpid()));
  const auto directory = root / "folder with spaces # and %";
  fs::create_directories(directory);
  std::ofstream(root / "file") << "test";
  assert(filemanager::startupPath(directory.string()) == directory.string());
  gchar* uri = g_filename_to_uri(directory.c_str(), nullptr, nullptr);
  assert(uri);
  assert(filemanager::startupPath(uri) == directory.string());
  g_free(uri);
  uri = g_filename_to_uri(directory.c_str(), "localhost", nullptr);
  assert(filemanager::startupPath(uri) == directory.string());
  g_free(uri);
  const auto previous = fs::current_path();
  fs::current_path(root);
  assert(filemanager::startupPath(directory.filename().string()) == directory.string());
  assert(filemanager::startupPath("") == g_get_home_dir());
  fs::current_path(previous);
  for (const auto& invalid : {(root / "missing").string(), (root / "file").string(),
                              std::string("file:///bad%ZZ"), std::string("file://remote/tmp"),
                              std::string("https://example.com/")}) {
    bool rejected = false;
    try {
      filemanager::startupPath(invalid);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    assert(rejected);
  }
  fs::remove_all(root);
}
