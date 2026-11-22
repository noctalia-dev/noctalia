#include "net/url_open.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  using namespace std::chrono_literals;

  template <typename Predicate> bool waitUntil(Predicate&& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (std::invoke(ready)) {
        return true;
      }
      std::this_thread::sleep_for(5ms);
    }
    return std::invoke(ready);
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-url-open-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root / "data/applications");
  std::filesystem::create_directories(root / "config");
  ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
  ::setenv("XDG_DATA_DIRS", (root / "data").c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_CONFIG_DIRS", (root / "config").c_str(), 1);
  ::setenv("GIO_USE_PORTALS", "1", 1);
  ::setenv("NIXOS_XDG_OPEN_USE_PORTAL", "1", 1);
  ::setenv("XDG_ACTIVATION_TOKEN", "parent-token", 1);
  ::setenv("DESKTOP_STARTUP_ID", "parent-startup-id", 1);

  const auto output = root / "launch.txt";
  const auto script = root / "browser.sh";
  {
    std::ofstream file(script);
    file
        << "#!/bin/sh\nprintf '%s\\n%s\\n%s' \"$XDG_ACTIVATION_TOKEN\" \"$DESKTOP_STARTUP_ID\" \"$1\" > '"
        << output.string()
        << "'\n";
  }
  {
    std::ofstream file(root / "data/applications/noctalia-url-test.desktop");
    file
        << "[Desktop Entry]\nType=Application\nName=URL test browser\nExec=/bin/sh "
        << script.string()
        << " %u\nStartupNotify=true\nMimeType=x-scheme-handler/https;inode/directory;\n";
  }
  {
    std::ofstream file(root / "config/mimeapps.list");
    file
        << "[Default Applications]\nx-scheme-handler/https=noctalia-url-test.desktop\n"
        << "inode/directory=noctalia-url-test.desktop\n";
  }

  auto connection = sdbus::createSessionBusConnection(sdbus::ServiceName{"org.freedesktop.portal.Desktop"});
  auto portal = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/freedesktop/portal/desktop"});
  std::mutex mutex;
  std::string receivedUri;
  std::string receivedToken;
  portal
      ->addVTable(
          sdbus::registerMethod("OpenURI").implementedAs([&](const std::string&, const std::string& uri,
                                                             const std::map<std::string, sdbus::Variant>& options) {
            std::scoped_lock lock(mutex);
            receivedUri = uri;
            if (const auto token = options.find("activation_token"); token != options.end()) {
              receivedToken = token->second.get<std::string>();
            }
            return sdbus::ObjectPath{"/org/freedesktop/portal/desktop/request/test/open"};
          })
      )
      .forInterface("org.freedesktop.portal.OpenURI");

  // Owning the name keeps revealFile from D-Bus-activating the developer's real file manager.
  connection->requestName(sdbus::ServiceName{"org.freedesktop.FileManager1"});
  auto fileManager = sdbus::createObject(*connection, sdbus::ObjectPath{"/org/freedesktop/FileManager1"});
  std::vector<std::string> shownUris;
  std::string shownStartupId;
  bool failShowItems = false;
  fileManager
      ->addVTable(
          sdbus::registerMethod("ShowItems")
              .implementedAs([&](const std::vector<std::string>& uris, const std::string& startupId) {
                std::scoped_lock lock(mutex);
                if (failShowItems) {
                  throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "stub failure");
                }
                shownUris = uris;
                shownStartupId = startupId;
              })
      )
      .forInterface("org.freedesktop.FileManager1");
  connection->enterEventLoopAsync();

  // An unregistered scheme exercises GIO's portal route on the private test bus.
  const std::string portalUri = "noctalia-url-test://template/readme";
  TEST_CHECK(net::openInBrowser(portalUri, "portal-token"));
  TEST_CHECK(waitUntil([&] {
    std::scoped_lock lock(mutex);
    return receivedUri == portalUri;
  }));
  {
    std::scoped_lock lock(mutex);
    TEST_CHECK(receivedToken == "portal-token");
  }

  const std::string directUri = "https://example.invalid/template?value=a%20b&quoted='value'";
  TEST_CHECK(net::openInBrowser(directUri, "direct-token"));
  TEST_CHECK(waitUntil([&] { return readFile(output) == "direct-token\ndirect-token\n" + directUri; }));
  TEST_CHECK(std::string(std::getenv("XDG_ACTIVATION_TOKEN")) == "parent-token");
  TEST_CHECK(std::string(std::getenv("DESKTOP_STARTUP_ID")) == "parent-startup-id");

  TEST_CHECK(net::openInBrowser(directUri));
  TEST_CHECK(waitUntil([&] { return readFile(output) == "\n\n" + directUri; }));

  // revealFile selects the file through FileManager1, escaping the path into a file URI.
  const auto clip = root / "clip one.mp4";
  TEST_CHECK(net::revealFile(clip, "reveal-token"));
  TEST_CHECK(waitUntil([&] {
    std::scoped_lock lock(mutex);
    return shownUris == std::vector<std::string>{"file://" + root.string() + "/clip%20one.mp4"};
  }));
  {
    std::scoped_lock lock(mutex);
    TEST_CHECK(shownStartupId == "reveal-token");
    failShowItems = true;
  }

  // Without a working file manager it opens the parent folder with the default handler instead. GIO hands
  // local files to %u as plain paths.
  TEST_CHECK(net::revealFile(clip, "fallback-token"));
  TEST_CHECK(waitUntil([&] { return readFile(output) == "fallback-token\nfallback-token\n" + root.string(); }));

  TEST_CHECK(!net::revealFile("relative.mp4"));

  connection->leaveEventLoop();
  std::filesystem::remove_all(root);
}
