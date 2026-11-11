#include "system/icon_resolver.h"
#include "tests/test_check.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  // Removed at exit, so a failed check does not leave the fixture behind.
  fs::path tempRoot;

  void removeTempRoot() {
    std::error_code ec;
    fs::permissions(tempRoot / "denied", fs::perms::owner_all, ec);
    fs::remove_all(tempRoot, ec);
  }

} // namespace

int main() {
  char tempDir[] = "/tmp/noctalia-icon-resolver-test-XXXXXX";
  if (mkdtemp(tempDir) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }
  tempRoot = tempDir;
  std::atexit(removeTempRoot);

  const fs::path root(tempDir);
  const fs::path iconThemeRoot = root / "icons/hicolor";
  const fs::path iconDir = iconThemeRoot / "scalable/apps";
  const fs::path app16Dir = iconThemeRoot / "16x16/apps";
  const fs::path bitmapIconDir = iconThemeRoot / "48x48/apps";
  const fs::path status16Dir = iconThemeRoot / "16x16/status";
  const fs::path status24Dir = iconThemeRoot / "24x24/status";
  const fs::path panel16Dir = iconThemeRoot / "16x16/panel";
  const fs::path panel24Dir = iconThemeRoot / "24x24/panel";
  const fs::path scalableStatusDir = iconThemeRoot / "scalable/status";
  const fs::path defaultScalableDir = iconThemeRoot / "default/status";
  const fs::path deniedDataHome = root / "denied";
  const fs::path deniedIcon = deniedDataHome / "private-icon.svg";
  fs::create_directories(iconDir);
  fs::create_directories(app16Dir);
  fs::create_directories(bitmapIconDir);
  const fs::path regularPairIcon = iconDir / "symbolic-pair.svg";
  const fs::path symbolicPairIcon = iconDir / "symbolic-pair-symbolic.svg";
  const fs::path regularOnlyIcon = iconDir / "regular-only.svg";
  std::ofstream(regularPairIcon) << "<svg/>";
  std::ofstream(symbolicPairIcon) << "<svg/>";
  std::ofstream(regularOnlyIcon) << "<svg/>";
  fs::create_directories(status16Dir);
  fs::create_directories(status24Dir);
  fs::create_directories(panel16Dir);
  fs::create_directories(panel24Dir);
  fs::create_directories(scalableStatusDir);
  fs::create_directories(defaultScalableDir);
  const std::string indexContents = "[Icon Theme]\n"
                                    "Directories = 48x48/apps, scalable/apps, 16x16/apps, 16x16/status, 24x24/status, "
                                    "16x16/panel, 24x24/panel, scalable/status, default/status\n"
                                    "Inherits = noctalia-status-test, noctalia-bare-test\n"
                                    "[48x48/apps]\n"
                                    "Size = 48\n"
                                    "Type = Fixed\n"
                                    "[scalable/apps]\n"
                                    "Size = 64\n"
                                    "Type = Scalable\n"
                                    "MaxSize = 128\n"
                                    "[16x16/apps]\n"
                                    "Size = 16\n"
                                    "Type = Fixed\n"
                                    "[16x16/status]\n"
                                    "Size = 16\n"
                                    "Type = Fixed\n"
                                    "[24x24/status]\n"
                                    "Size = 24\n"
                                    "Type = Fixed\n"
                                    "[16x16/panel]\n"
                                    "Size = 16\n"
                                    "Type = Threshold\n"
                                    "[24x24/panel]\n"
                                    "Size = 24\n"
                                    "Threshold = 3\n"
                                    "[scalable/status]\n"
                                    "Size = 48\n"
                                    "Type = Scalable\n"
                                    "MinSize = 32\n"
                                    "MaxSize = 64\n"
                                    "[default/status]\n"
                                    "Size = 24\n"
                                    "Type = Scalable\n";
  std::ofstream(iconThemeRoot / "index.theme") << indexContents;
  const fs::path status16 = status16Dir / "test-status-symbolic.svg";
  const fs::path status24 = status24Dir / "test-status-symbolic.svg";
  const fs::path ordinaryIcon = iconDir / "ordinary-icon.svg";
  const fs::path symbolicApp16 = app16Dir / "test-app-symbolic.svg";
  const fs::path symbolicAppScalable = iconDir / "test-app-symbolic.svg";
  const fs::path bitmapStatus = bitmapIconDir / "bitmap-status.png";
  std::ofstream(status16) << "<svg/>";
  std::ofstream(status24) << "<svg/>";
  std::ofstream(ordinaryIcon) << "<svg/>";
  std::ofstream(symbolicApp16) << "<svg/>";
  std::ofstream(symbolicAppScalable) << "<svg/>";
  std::ofstream(bitmapStatus) << "png";

  std::ofstream(panel16Dir / "threshold.svg") << "<svg/>";
  std::ofstream(panel24Dir / "threshold.svg") << "<svg/>";
  std::ofstream(panel24Dir / "range-distance.svg") << "<svg/>";
  std::ofstream(status16Dir / "range-distance.svg") << "<svg/>";
  std::ofstream(panel16Dir / "scalable-range.svg") << "<svg/>";
  std::ofstream(scalableStatusDir / "scalable-range.svg") << "<svg/>";
  std::ofstream(scalableStatusDir / "scalable-only.svg") << "<svg/>";
  std::ofstream(status24Dir / "split-root.svg") << "<svg/>";
  std::ofstream(defaultScalableDir / "scalable-max-default.svg") << "<svg/>";
  std::ofstream(panel24Dir / "scalable-max-default.svg") << "<svg/>";
  std::ofstream(defaultScalableDir / "scalable-defaults.svg") << "<svg/>";
  std::ofstream(status16Dir / "scalable-defaults.svg") << "<svg/>";
  std::ofstream(status16Dir / "bitmap-only.png") << "png";

  // An inherited theme with no index.theme exercises the fallback search paths
  // (theme root and 512x512/apps) that only apply to index-less themes.
  const fs::path bareThemeRoot = root / "icons/noctalia-bare-test";
  const fs::path bareBitmapDir = bareThemeRoot / "512x512/apps";
  fs::create_directories(bareBitmapDir);
  const fs::path bareSizedIcon = bareBitmapDir / "bare-sized-icon.png";
  const fs::path bareRootIcon = bareThemeRoot / "bare-root-icon.png";
  std::ofstream(bareSizedIcon) << "png";
  std::ofstream(bareRootIcon) << "png";
  const fs::path themedBitmap = bitmapIconDir / "theme-priority.png";
  std::ofstream(themedBitmap) << "png";
  std::ofstream(bareThemeRoot / "theme-priority.svg") << "<svg/>";
  std::ofstream(bitmapIconDir / "bitmap-priority.png") << "png";
  std::ofstream(bareBitmapDir / "bitmap-priority.png") << "png";
  const fs::path extraData = root / "extra";
  const fs::path extraIcons = extraData / "icons/hicolor/scalable/apps";
  fs::create_directories(extraIcons);
  std::ofstream(extraIcons / "later-root.svg") << "<svg/>";
  std::ofstream(bareThemeRoot / "later-root.svg") << "<svg/>";
  const fs::path extraThemeRoot = extraData / "icons/hicolor";
  const fs::path extraStatusDir = extraThemeRoot / "16x16/status";
  fs::create_directories(extraStatusDir);
  std::ofstream(extraStatusDir / "split-root.svg") << "<svg/>";
  const fs::path statusThemeRoot = root / "icons/noctalia-status-test";
  const fs::path inheritedStatusDir = statusThemeRoot / "16x16/status";
  fs::create_directories(inheritedStatusDir);
  std::ofstream(statusThemeRoot / "index.theme") << "[Icon Theme]\nDirectories = 16x16/status\n"
                                                    "[16x16/status]\nSize = 16\nType = Fixed\n";
  std::ofstream(inheritedStatusDir / "test-app-symbolic.svg") << "<svg/>";
  std::ofstream(inheritedStatusDir / "bitmap-only.svg") << "<svg/>";
  std::ofstream(inheritedStatusDir / "inherited-status.svg") << "<svg/>";
  fs::create_directories(deniedDataHome);
  std::ofstream(deniedIcon) << "<svg/>";
  fs::permissions(deniedDataHome, fs::perms::none);
  // Unthemed icons at the top of base dirs (XDG LookupFallbackIcon).
  fs::create_directories(root / ".icons");
  fs::create_directories(root / "pixmaps");
  std::ofstream(root / "icons/unthemed-app.png") << "png";
  std::ofstream(root / ".icons/home-unthemed.svg") << "<svg/>";
  std::ofstream(root / "icons/regular-only.png") << "png";
  std::ofstream(root / "icons/fallback-order.png") << "png";
  std::ofstream(root / "pixmaps/fallback-order.png") << "png";
  setenv("HOME", tempDir, 1);
  setenv("XDG_DATA_HOME", deniedDataHome.c_str(), 1);
  setenv("XDG_DATA_DIRS", (root.string() + ":" + extraData.string()).c_str(), 1);

  std::array<int, 2> logPipe{-1, -1};
  TEST_CHECK(::pipe(logPipe.data()) == 0);
  std::fflush(stderr);
  const int savedStderr = ::dup(STDERR_FILENO);
  TEST_CHECK(savedStderr >= 0 && ::dup2(logPipe[1], STDERR_FILENO) >= 0);

  IconResolver resolver(true);
  const auto initialGeneration = IconResolver::themeGeneration();
  std::ofstream(iconThemeRoot / "index.theme") << indexContents;
  // Checked once stderr is restored, so a failure message is not captured.
  const bool equivalentRewriteChangedPlan = IconResolver::checkThemePlanChanged();
  const bool generationPreserved = IconResolver::themeGeneration() == initialGeneration;
  std::fflush(stderr);
  (void)::dup2(savedStderr, STDERR_FILENO);
  ::close(savedStderr);
  ::close(logPipe[1]);

  std::string startupLogs;
  std::array<char, 1024> logBuffer{};
  ssize_t count = 0;
  while ((count = ::read(logPipe[0], logBuffer.data(), logBuffer.size())) > 0) {
    startupLogs.append(logBuffer.data(), static_cast<std::size_t>(count));
  }
  ::close(logPipe[0]);

  // An equivalent theme metadata rewrite should be ignored.
  TEST_CHECK(!equivalentRewriteChangedPlan);
  // An equivalent theme metadata rewrite should preserve the theme generation.
  TEST_CHECK(generationPreserved);

  // Status vector should use the matching 16px theme asset.
  TEST_CHECK(resolver.resolveStatusVector("test-status-symbolic", 16) == status16.string());
  // Status vector should use the matching 24px theme asset.
  TEST_CHECK(resolver.resolveStatusVector("test-status-symbolic", 24) == status24.string());
  // Ordinary application icons should keep normal resolution.
  TEST_CHECK(
      resolver.resolveStatusVector("ordinary-icon", 16).empty()
      && resolver.resolve("ordinary-icon", 32) == ordinaryIcon.string()
  );
  // Symbolic application icons should keep normal resolution.
  TEST_CHECK(
      resolver.resolveStatusVector("test-app-symbolic", 16).empty()
      && resolver.resolve("test-app-symbolic", 32) == symbolicAppScalable.string()
  );
  // Bitmap status icons should keep the larger raster request.
  TEST_CHECK(
      resolver.resolveStatusVector("bitmap-status", 16).empty()
      && resolver.resolve("bitmap-status", 32) == bitmapStatus.string()
  );

  for (const int size : {14, 16, 18, 19}) {
    // 16px Threshold art should match its default tolerance and nearest range.
    TEST_CHECK(resolver.resolveStatusVector("threshold", size) == (panel16Dir / "threshold.svg").string());
  }
  for (const int size : {20, 21, 24, 27, 28}) {
    // 24px default-Type art should use its explicit Threshold range.
    TEST_CHECK(resolver.resolveStatusVector("threshold", size) == (panel24Dir / "threshold.svg").string());
  }
  // Distance to a Threshold range should precede distance to nominal size.
  TEST_CHECK(resolver.resolveStatusVector("range-distance", 19) == (panel24Dir / "range-distance.svg").string());
  // Matching scalable range should precede an undersized Threshold icon.
  TEST_CHECK(resolver.resolveStatusVector("scalable-range", 32) == (scalableStatusDir / "scalable-range.svg").string());
  // Matching Threshold range should precede an oversized scalable range.
  TEST_CHECK(resolver.resolveStatusVector("scalable-range", 16) == (panel16Dir / "scalable-range.svg").string());
  // Scalable status art should remain available outside its preferred range.
  TEST_CHECK(resolver.resolveStatusVector("scalable-only", 16) == (scalableStatusDir / "scalable-only.svg").string());
  // Status lookup should share theme metadata with an index-less second root.
  TEST_CHECK(resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string());
  // Current-theme bitmap must precede inherited status SVG.
  TEST_CHECK(
      resolver.resolveStatusVector("bitmap-only", 16).empty()
      && resolver.resolve("bitmap-only", 32) == (status16Dir / "bitmap-only.png").string()
  );
  // Inherited status art should resolve when the current theme lacks the name.
  TEST_CHECK(
      resolver.resolveStatusVector("inherited-status", 16) == (inheritedStatusDir / "inherited-status.svg").string()
  );

  // Omitted scalable bounds should default to nominal Size.
  TEST_CHECK(
      resolver.resolveStatusVector("scalable-defaults", 32) == (defaultScalableDir / "scalable-defaults.svg").string()
  );
  // Default scalable bounds should not match every requested size.
  TEST_CHECK(resolver.resolveStatusVector("scalable-defaults", 17) == (status16Dir / "scalable-defaults.svg").string());
  // Omitted scalable maximum should not extend beyond nominal Size.
  TEST_CHECK(
      resolver.resolveStatusVector("scalable-max-default", 26) == (panel24Dir / "scalable-max-default.svg").string()
  );
  std::string widerThreshold = indexContents;
  const auto thresholdSection = widerThreshold.find("[16x16/panel]\nSize = 16\nType = Threshold\n");
  widerThreshold.insert(
      thresholdSection + std::string("[16x16/panel]\nSize = 16\nType = Threshold\n").size(), "Threshold = 5\n"
  );
  const auto rangeGeneration = IconResolver::themeGeneration();
  std::ofstream(iconThemeRoot / "index.theme") << widerThreshold;
  // Changing only a Threshold range should refresh the theme plan.
  TEST_CHECK(IconResolver::checkThemePlanChanged());
  // Range changes should advance the theme generation.
  TEST_CHECK(IconResolver::themeGeneration() > rangeGeneration);
  // Range metadata changes should refresh status selection.
  TEST_CHECK(resolver.resolveStatusVector("threshold", 20) == (panel16Dir / "threshold.svg").string());
  std::ofstream(iconThemeRoot / "index.theme") << indexContents;
  // Restoring the Threshold range should refresh the theme plan.
  TEST_CHECK(IconResolver::checkThemePlanChanged());

  const bool permissionsEnforced = (geteuid() != 0);
  if (permissionsEnforced) {
    // An inaccessible icon directory should be logged.
    TEST_CHECK(startupLogs.contains(deniedDataHome.string()));
    // An icon beneath an inaccessible directory should not terminate resolution.
    TEST_CHECK(resolver.resolve(deniedIcon.string(), 32).empty());
    fs::permissions(deniedDataHome, fs::perms::owner_all);
  }
  // A missing icon directory should not be logged.
  TEST_CHECK(!startupLogs.contains((root / ".icons/hicolor").string()));
  // An absolute icon denied earlier should resolve once it is readable.
  TEST_CHECK(resolver.resolve(deniedIcon.string(), 32) == deniedIcon.string());

  const fs::path invalidatedIcon = iconDir / "invalidated-icon.svg";
  // Initial missing icon should not resolve.
  TEST_CHECK(resolver.resolve("invalidated-icon", 32).empty());
  std::ofstream(invalidatedIcon) << "<svg/>";
  // Named icon miss should be cached.
  TEST_CHECK(resolver.resolve("invalidated-icon", 32).empty());
  resolver.invalidateMissingCache();
  // Invalidating misses should discover a newly created icon.
  TEST_CHECK(resolver.resolve("invalidated-icon", 32) == invalidatedIcon.string());

  const fs::path polledIcon = iconDir / "polled-icon.svg";
  // Second initial icon miss should be cached.
  TEST_CHECK(resolver.resolve("polled-icon", 32).empty());
  std::ofstream(polledIcon) << "<svg/>";
  IconResolver::invalidateThemeContent();
  // Icon content invalidation should clear cached misses.
  TEST_CHECK(resolver.resolve("polled-icon", 32) == polledIcon.string());

  // Normal resolution should keep the regular icon.
  TEST_CHECK(resolver.resolve("symbolic-pair", 32) == regularPairIcon.string());
  const ResolvedIcon preferred = resolver.resolveSymbolicPreferred("symbolic-pair", 32);
  // Symbolic-preferred resolution should select the -symbolic variant.
  TEST_CHECK(preferred.path == symbolicPairIcon.string() && preferred.symbolic);

  const ResolvedIcon fallback = resolver.resolveSymbolicPreferred("regular-only", 32);
  // Symbolic-preferred resolution should fall back to the regular icon.
  TEST_CHECK(fallback.path == regularOnlyIcon.string() && !fallback.symbolic);

  const fs::path lateSymbolicIcon = iconDir / "regular-only-symbolic.svg";
  std::ofstream(lateSymbolicIcon) << "<svg/>";
  const ResolvedIcon cachedFallback = resolver.resolveSymbolicPreferred("regular-only", 32);
  // Preferred resolution should cache a successful regular fallback.
  TEST_CHECK(cachedFallback.path == regularOnlyIcon.string() && !cachedFallback.symbolic);
  resolver.invalidateMissingCache();
  const ResolvedIcon refreshedPreferred = resolver.resolveSymbolicPreferred("regular-only", 32);
  // Invalidating misses should also refresh symbolic preference results.
  TEST_CHECK(refreshedPreferred.path == lateSymbolicIcon.string() && refreshedPreferred.symbolic);

  const fs::path absoluteIcon = root / "absolute-icon.svg";
  // Missing absolute icon should not resolve.
  TEST_CHECK(resolver.resolve(absoluteIcon.string(), 32).empty());
  std::ofstream(absoluteIcon) << "<svg/>";
  // Absolute icon misses should not be cached.
  TEST_CHECK(resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string());
  // Absolute icon should be cached while present.
  TEST_CHECK(resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string());
  std::error_code ec;
  fs::remove(absoluteIcon, ec);
  // Deleted absolute icon should not stay cached.
  TEST_CHECK(resolver.resolve(absoluteIcon.string(), 32).empty());
  std::ofstream(absoluteIcon) << "<svg/>";
  // Recreated absolute icon should resolve after cache eviction.
  TEST_CHECK(resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string());

  // Index-less inherited theme should resolve icons under 512x512/apps.
  TEST_CHECK(resolver.resolve("bare-sized-icon", 32) == bareSizedIcon.string());
  // Index-less inherited theme should resolve icons at the theme root.
  TEST_CHECK(resolver.resolve("bare-root-icon", 32) == bareRootIcon.string());

  for (const int size : {0, 32, 64, 128}) {
    // Current-theme bitmap must precede a better-sized inherited bitmap.
    TEST_CHECK(resolver.resolve("bitmap-priority", size) == (bitmapIconDir / "bitmap-priority.png").string());
    // Current-theme bitmap must precede an inherited vector icon.
    TEST_CHECK(resolver.resolve("theme-priority", size) == themedBitmap.string());
    // All roots of the current theme must precede inherited themes.
    TEST_CHECK(resolver.resolve("later-root", size) == (extraIcons / "later-root.svg").string());
  }

  for (const int size : {0, 48}) {
    // An icon at the top of a data dir icons root should resolve.
    TEST_CHECK(resolver.resolve("unthemed-app", size) == (root / "icons/unthemed-app.png").string());
    // An icon at the top of ~/.icons should resolve.
    TEST_CHECK(resolver.resolve("home-unthemed", size) == (root / ".icons/home-unthemed.svg").string());
    // A themed icon must precede an unthemed icon of the same name.
    TEST_CHECK(resolver.resolve("regular-only", size) == regularOnlyIcon.string());
    // Unthemed base dir icons must precede pixmaps.
    TEST_CHECK(resolver.resolve("fallback-order", size) == (root / "icons/fallback-order.png").string());
  }

  setenv("XDG_DATA_DIRS", (extraData.string() + ":" + root.string()).c_str(), 1);
  IconResolver::checkThemePlanChanged();
  // An index-less first root should use metadata from the next theme root.
  TEST_CHECK(resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string());
  setenv("XDG_DATA_DIRS", (root.string() + ":" + extraData.string()).c_str(), 1);
  std::ofstream(extraThemeRoot / "index.theme")
      << "[Icon Theme]\nDirectories = 24x24/status\nInherits = conflicting-parent\n"
         "[24x24/status]\nSize = 24\nType = Fixed\n";
  IconResolver::checkThemePlanChanged();
  // Later-root metadata should not replace the first index.theme directory list.
  TEST_CHECK(resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string());

  return 0;
}
