#include "filemanager/startup_path.h"
#include "test_isolation.h"

#include <fstream>
#include <unistd.h>

int main() {
  return nythera::test::runTest([] {
    namespace fs = std::filesystem;
    nythera::test::ScopedTestEnv env({.prefix = "nythera-startup"});
    const auto& root = env.work();
    const auto directory = root / "folder with spaces # and %";
    fs::create_directories(directory);
    std::ofstream(root / "file") << "test";
    TEST_CHECK(filemanager::startupPath(directory.string()) == directory.string());
    gchar* uri = g_filename_to_uri(directory.c_str(), nullptr, nullptr);
    TEST_CHECK(uri != nullptr);
    TEST_CHECK(filemanager::startupPath(uri) == directory.string());
    g_free(uri);
    uri = g_filename_to_uri(directory.c_str(), "localhost", nullptr);
    TEST_CHECK(uri != nullptr);
    TEST_CHECK(filemanager::startupPath(uri) == directory.string());
    g_free(uri);
    {
      nythera::test::ScopedCurrentPath restoreCwd;
      fs::current_path(root);
      TEST_CHECK(filemanager::startupPath(directory.filename().string()) == directory.string());
      TEST_CHECK(filemanager::startupPath("") == g_get_home_dir());
    }
    for (const auto& invalid :
         {(root / "missing").string(), (root / "file").string(), std::string("file:///bad%ZZ"),
          std::string("file://remote/tmp"), std::string("https://example.com/")}) {
      bool rejected = false;
      try {
        filemanager::startupPath(invalid);
      } catch (const std::runtime_error&) {
        rejected = true;
      }
      TEST_CHECK(rejected);
    }
  });
}
