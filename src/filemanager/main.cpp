#include "core/files/resource_paths.h"
#include "filemanager/file_manager.h"
#include "filemanager/startup_path.h"
#include "net/http_client.h"

#include <exception>
#include <print>

int main(int argc, char** argv) {
  try {
    if (argc > 2)
      throw std::runtime_error("Expected at most one local directory path or file URI");
    paths::setInstallPaths(NYTHERA_INSTALL_PREFIX, NYTHERA_INSTALL_DATADIR);
    HttpClient::setUserAgent("nythera/" NYTHERA_VERSION);
    paths::selectFilesAssets();
    FileManager manager(filemanager::startupPath(argc > 1 ? argv[1] : ""));
    return manager.run();
  } catch (const std::exception& error) {
    std::println(stderr, "nythera-file-manager: {}", error.what());
    return 1;
  }
}
