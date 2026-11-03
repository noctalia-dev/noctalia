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
  // A theme inside a system icon root is still system-installed: issue #4338 is apps
  // advertising <datadir>/icons/hicolor.
  TEST_CHECK(tray::isSystemIconRoot(root / "share/icons/hicolor"));
  TEST_CHECK(tray::isSystemIconRoot(root / "share/icons/hicolor/48x48/apps"));
  TEST_CHECK(tray::isSystemIconRoot(root / "share/pixmaps/vendor"));
  // An app-private tree that merely happens to be named "icons" must still be indexed.
  TEST_CHECK(!tray::isSystemIconRoot(root / "app/icons"));
  TEST_CHECK(!tray::isSystemIconRoot(root / "app/icons/hicolor"));
  TEST_CHECK(!tray::isSystemIconRoot(root / "share/iconsets"));

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

  // A second bar's TrayWidget shares the index instead of re-walking the tree: a file
  // added after the scan stays invisible, so no second walk happened.
  std::ofstream(appTheme / "48x48/apps/late-icon.png") << "x";
  TEST_CHECK(
      store.resolve(appTheme.string(), "deep-icon") == (appTheme / "hicolor/48x48/apps/symbolic/deep-icon.png").string()
  );
  for (int i = 0; i < 20; ++i) {
    TEST_CHECK(store.resolve(appTheme.string(), "late-icon").empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

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

  // A listener that removes another one must stop that one from firing in the same
  // batch: its owner may already be destroyed by the time the batch reaches it.
  int firedA = 0;
  int firedB = 0;
  std::uint64_t idA = 0;
  std::uint64_t idB = 0;
  idA = store.addListener([&]() {
    ++firedA;
    store.removeListener(idB);
  });
  idB = store.addListener([&]() {
    ++firedB;
    store.removeListener(idA);
  });

  TEST_CHECK(store.resolve((root / "app/second").string(), "shallow-icon").empty());
  for (int i = 0; i < 500 && firedA + firedB == 0; ++i) {
    for (const auto& pending : DeferredCall::takePending()) {
      pending();
    }
    if (firedA + firedB == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  TEST_CHECK(firedA + firedB == 1);

  store.removeListener(idA);
  store.removeListener(idB);

  fs::remove_all(root);
}
