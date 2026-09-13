#include "core/deferred_call.h"
#include "shell/tray/tray_theme_path_index.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  char tempDir[] = "/tmp/noctalia-tray-theme-path-test-XXXXXX";
  if (mkdtemp(tempDir) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  const fs::path root(tempDir);

  fs::create_directories(root / "share/icons");
  fs::create_directories(root / "share/pixmaps");
  fs::create_directories(root / "app/icons");
  fs::create_directories(root / "sealed/share/icons");
  fs::create_directory_symlink(root / "share/icons", root / "twin-icons");
  setenv("XDG_DATA_HOME", (root / "share").c_str(), 1);
  setenv("XDG_DATA_DIRS", ((root / "share").string() + ":" + (root / "sealed/share").string()).c_str(), 1);

  TEST_CHECK(tray::isSystemIconRoot(root / "share/icons"));
  TEST_CHECK(tray::isSystemIconRoot(root / "share/pixmaps"));
  TEST_CHECK(tray::isSystemIconRoot(root / "share/icons/"));
  TEST_CHECK(tray::isSystemIconRoot(root / "share/./icons/../icons"));
  TEST_CHECK(tray::isSystemIconRoot(root / "twin-icons"));
  TEST_CHECK(!tray::isSystemIconRoot(root / "app/icons"));
  TEST_CHECK(!tray::isSystemIconRoot(root / "share/icons/hicolor"));

  // A data dir that cannot be canonicalized must still match by its raw form.
  fs::permissions(root / "sealed", fs::perms::none);
  if (::geteuid() != 0) {
    std::error_code ec;
    (void)fs::weakly_canonical(root / "sealed/share/icons", ec);
    TEST_CHECK(static_cast<bool>(ec));
    TEST_CHECK(tray::isSystemIconRoot(root / "sealed/share/icons"));
  }
  fs::permissions(root / "sealed", fs::perms::owner_all);

  const fs::path appTheme = root / "app/theme";
  fs::create_directories(appTheme / "48x48/apps");
  fs::create_directories(appTheme / "hicolor/48x48/apps/symbolic");
  fs::create_directories(appTheme / "a/b/c/d/e/f/g");
  fs::create_directories(appTheme / "cursors/Adwaita/cursors");
  std::ofstream(appTheme / "48x48/apps/shallow-icon.png") << "x";
  std::ofstream(appTheme / "hicolor/48x48/apps/symbolic/deep-icon.png") << "x";
  std::ofstream(appTheme / "a/b/c/d/e/f/g/buried-icon.png") << "x";
  std::ofstream(appTheme / "cursors/Adwaita/cursors/cursor-icon.png") << "x";
  std::ofstream(appTheme / "48x48/apps/not-an-icon.txt") << "x";

  const tray::ThemePathIndex index = tray::buildThemePathIndex(appTheme);
  TEST_CHECK(index.contains("shallow-icon"));
  TEST_CHECK(index.contains("deep-icon"));
  TEST_CHECK(index.at("deep-icon") == (appTheme / "hicolor/48x48/apps/symbolic/deep-icon.png").string());
  TEST_CHECK(!index.contains("buried-icon"));
  TEST_CHECK(!index.contains("cursor-icon"));
  TEST_CHECK(!index.contains("not-an-icon"));

  TEST_CHECK(tray::buildThemePathIndex(root / "share/icons").empty());

  const fs::path flood = root / "app/flood";
  fs::create_directories(flood);
  constexpr int kFloodFiles = 20050;
  for (int i = 0; i < kFloodFiles; ++i) {
    std::ofstream(flood / ("flood" + std::to_string(i) + ".png")) << "x";
  }
  const tray::ThemePathIndex flooded = tray::buildThemePathIndex(flood);
  int found = 0;
  for (int i = 0; i < kFloodFiles; ++i) {
    found += static_cast<int>(flooded.contains("flood" + std::to_string(i)));
  }
  TEST_CHECK(found <= 20000);
  TEST_CHECK(found >= 19000);

  auto& store = tray::ThemePathIconStore::instance();
  int notified = 0;
  const std::uint64_t listener = store.addListener([&notified]() { ++notified; });

  TEST_CHECK(store.resolve(appTheme.string(), "shallow-icon").empty());
  TEST_CHECK(notified == 0);

  std::string resolved;
  for (int i = 0; i < 500 && resolved.empty(); ++i) {
    resolved = store.resolve(appTheme.string(), "shallow-icon");
    if (resolved.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  TEST_CHECK(resolved == (appTheme / "48x48/apps/shallow-icon.png").string());
  TEST_CHECK(store.scansPerformed() == 1);

  // A second bar's TrayWidget shares the index instead of re-walking the tree.
  TEST_CHECK(
      store.resolve(appTheme.string(), "deep-icon") == (appTheme / "hicolor/48x48/apps/symbolic/deep-icon.png").string()
  );
  TEST_CHECK(store.scansPerformed() == 1);

  for (int i = 0; i < 500 && notified == 0; ++i) {
    for (const auto& pending : DeferredCall::takePending()) {
      pending();
    }
    if (notified == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  TEST_CHECK(notified == 1);

  store.removeListener(listener);
  TEST_CHECK(store.resolve((root / "app/missing").string(), "shallow-icon").empty());
  for (int i = 0; i < 50; ++i) {
    for (const auto& pending : DeferredCall::takePending()) {
      pending();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  TEST_CHECK(notified == 1);

  fs::remove_all(root);
}
