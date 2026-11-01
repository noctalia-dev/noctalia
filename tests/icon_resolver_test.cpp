#include "system/icon_resolver.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "icon_resolver_test: {}", message);
    }
    return condition;
  }

} // namespace

int main() {
  char tempDir[] = "/tmp/noctalia-icon-resolver-test-XXXXXX";
  if (mkdtemp(tempDir) == nullptr) {
    std::perror("mkdtemp");
    return 1;
  }

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

  bool ok = true;

  std::array<int, 2> logPipe{-1, -1};
  if (!expect(::pipe(logPipe.data()) == 0, "failed to create stderr capture pipe")) {
    return 1;
  }
  std::fflush(stderr);
  const int savedStderr = ::dup(STDERR_FILENO);
  if (!expect(savedStderr >= 0 && ::dup2(logPipe[1], STDERR_FILENO) >= 0, "failed to redirect stderr")) {
    ::close(logPipe[0]);
    ::close(logPipe[1]);
    if (savedStderr >= 0) {
      ::close(savedStderr);
    }
    return 1;
  }

  IconResolver resolver(true);
  const auto initialGeneration = IconResolver::themeGeneration();
  std::ofstream(iconThemeRoot / "index.theme") << indexContents;
  ok = expect(!IconResolver::checkThemePlanChanged(), "an equivalent theme metadata rewrite should be ignored") && ok;
  ok = expect(
           IconResolver::themeGeneration() == initialGeneration,
           "an equivalent theme metadata rewrite should preserve the theme generation"
       )
      && ok;
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

  ok = expect(
           resolver.resolveStatusVector("test-status-symbolic", 16) == status16.string(),
           "status vector should use the matching 16px theme asset"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("test-status-symbolic", 24) == status24.string(),
           "status vector should use the matching 24px theme asset"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("ordinary-icon", 16).empty()
               && resolver.resolve("ordinary-icon", 32) == ordinaryIcon.string(),
           "ordinary application icons should keep normal resolution"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("test-app-symbolic", 16).empty()
               && resolver.resolve("test-app-symbolic", 32) == symbolicAppScalable.string(),
           "symbolic application icons should keep normal resolution"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("bitmap-status", 16).empty()
               && resolver.resolve("bitmap-status", 32) == bitmapStatus.string(),
           "bitmap status icons should keep the larger raster request"
       )
      && ok;

  for (const int size : {14, 16, 18, 19}) {
    ok = expect(
             resolver.resolveStatusVector("threshold", size) == (panel16Dir / "threshold.svg").string(),
             "16px Threshold art should match its default tolerance and nearest range"
         )
        && ok;
  }
  for (const int size : {20, 21, 24, 27, 28}) {
    ok = expect(
             resolver.resolveStatusVector("threshold", size) == (panel24Dir / "threshold.svg").string(),
             "24px default-Type art should use its explicit Threshold range"
         )
        && ok;
  }
  ok = expect(
           resolver.resolveStatusVector("range-distance", 19) == (panel24Dir / "range-distance.svg").string(),
           "distance to a Threshold range should precede distance to nominal size"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("scalable-range", 32) == (scalableStatusDir / "scalable-range.svg").string(),
           "matching scalable range should precede an undersized Threshold icon"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("scalable-range", 16) == (panel16Dir / "scalable-range.svg").string(),
           "matching Threshold range should precede an oversized scalable range"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("scalable-only", 16) == (scalableStatusDir / "scalable-only.svg").string(),
           "scalable status art should remain available outside its preferred range"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string(),
           "status lookup should share theme metadata with an index-less second root"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("bitmap-only", 16).empty()
               && resolver.resolve("bitmap-only", 32) == (status16Dir / "bitmap-only.png").string(),
           "current-theme bitmap must precede inherited status SVG"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("inherited-status", 16)
               == (inheritedStatusDir / "inherited-status.svg").string(),
           "inherited status art should resolve when the current theme lacks the name"
       )
      && ok;

  ok = expect(
           resolver.resolveStatusVector("scalable-defaults", 32)
               == (defaultScalableDir / "scalable-defaults.svg").string(),
           "omitted scalable bounds should default to nominal Size"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("scalable-defaults", 17) == (status16Dir / "scalable-defaults.svg").string(),
           "default scalable bounds should not match every requested size"
       )
      && ok;
  ok = expect(
           resolver.resolveStatusVector("scalable-max-default", 26)
               == (panel24Dir / "scalable-max-default.svg").string(),
           "omitted scalable maximum should not extend beyond nominal Size"
       )
      && ok;
  std::string widerThreshold = indexContents;
  const auto thresholdSection = widerThreshold.find("[16x16/panel]\nSize = 16\nType = Threshold\n");
  widerThreshold.insert(
      thresholdSection + std::string("[16x16/panel]\nSize = 16\nType = Threshold\n").size(), "Threshold = 5\n"
  );
  const auto rangeGeneration = IconResolver::themeGeneration();
  std::ofstream(iconThemeRoot / "index.theme") << widerThreshold;
  ok = expect(IconResolver::checkThemePlanChanged(), "changing only a Threshold range should refresh the theme plan")
      && ok;
  ok = expect(IconResolver::themeGeneration() > rangeGeneration, "range changes should advance the theme generation")
      && ok;
  ok = expect(
           resolver.resolveStatusVector("threshold", 20) == (panel16Dir / "threshold.svg").string(),
           "range metadata changes should refresh status selection"
       )
      && ok;
  std::ofstream(iconThemeRoot / "index.theme") << indexContents;
  ok = expect(IconResolver::checkThemePlanChanged(), "restoring the Threshold range should refresh the theme plan")
      && ok;

  const bool permissionsEnforced = (geteuid() != 0);
  if (permissionsEnforced) {
    ok = expect(startupLogs.contains(deniedDataHome.string()), "an inaccessible icon directory should be logged") && ok;
    ok = expect(
             resolver.resolve(deniedIcon.string(), 32).empty(),
             "an icon beneath an inaccessible directory should not terminate resolution"
         )
        && ok;
    fs::permissions(deniedDataHome, fs::perms::owner_all);
  }
  ok =
      expect(!startupLogs.contains((root / ".icons/hicolor").string()), "a missing icon directory should not be logged")
      && ok;
  ok = expect(
           resolver.resolve(deniedIcon.string(), 32) == deniedIcon.string(),
           "an absolute icon denied earlier should resolve once it is readable"
       )
      && ok;

  const fs::path invalidatedIcon = iconDir / "invalidated-icon.svg";
  ok = expect(resolver.resolve("invalidated-icon", 32).empty(), "initial missing icon should not resolve") && ok;
  std::ofstream(invalidatedIcon) << "<svg/>";
  ok = expect(resolver.resolve("invalidated-icon", 32).empty(), "named icon miss should be cached") && ok;
  resolver.invalidateMissingCache();
  ok = expect(
           resolver.resolve("invalidated-icon", 32) == invalidatedIcon.string(),
           "invalidating misses should discover a newly created icon"
       )
      && ok;

  const fs::path polledIcon = iconDir / "polled-icon.svg";
  ok = expect(resolver.resolve("polled-icon", 32).empty(), "second initial icon miss should be cached") && ok;
  std::ofstream(polledIcon) << "<svg/>";
  IconResolver::invalidateThemeContent();
  ok = expect(
           resolver.resolve("polled-icon", 32) == polledIcon.string(),
           "icon content invalidation should clear cached misses"
       )
      && ok;

  ok = expect(
           resolver.resolve("symbolic-pair", 32) == regularPairIcon.string(),
           "normal resolution should keep the regular icon"
       )
      && ok;
  const ResolvedIcon preferred = resolver.resolveSymbolicPreferred("symbolic-pair", 32);
  ok = expect(
           preferred.path == symbolicPairIcon.string() && preferred.symbolic,
           "symbolic-preferred resolution should select the -symbolic variant"
       )
      && ok;

  const ResolvedIcon fallback = resolver.resolveSymbolicPreferred("regular-only", 32);
  ok = expect(
           fallback.path == regularOnlyIcon.string() && !fallback.symbolic,
           "symbolic-preferred resolution should fall back to the regular icon"
       )
      && ok;

  const fs::path lateSymbolicIcon = iconDir / "regular-only-symbolic.svg";
  std::ofstream(lateSymbolicIcon) << "<svg/>";
  const ResolvedIcon cachedFallback = resolver.resolveSymbolicPreferred("regular-only", 32);
  ok = expect(
           cachedFallback.path == regularOnlyIcon.string() && !cachedFallback.symbolic,
           "preferred resolution should cache a successful regular fallback"
       )
      && ok;
  resolver.invalidateMissingCache();
  const ResolvedIcon refreshedPreferred = resolver.resolveSymbolicPreferred("regular-only", 32);
  ok = expect(
           refreshedPreferred.path == lateSymbolicIcon.string() && refreshedPreferred.symbolic,
           "invalidating misses should also refresh symbolic preference results"
       )
      && ok;

  const fs::path absoluteIcon = root / "absolute-icon.svg";
  ok = expect(resolver.resolve(absoluteIcon.string(), 32).empty(), "missing absolute icon should not resolve") && ok;
  std::ofstream(absoluteIcon) << "<svg/>";
  ok = expect(
           resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string(),
           "absolute icon misses should not be cached"
       )
      && ok;
  ok = expect(
           resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string(),
           "absolute icon should be cached while present"
       )
      && ok;
  std::error_code ec;
  fs::remove(absoluteIcon, ec);
  ok =
      expect(resolver.resolve(absoluteIcon.string(), 32).empty(), "deleted absolute icon should not stay cached") && ok;
  std::ofstream(absoluteIcon) << "<svg/>";
  ok = expect(
           resolver.resolve(absoluteIcon.string(), 32) == absoluteIcon.string(),
           "recreated absolute icon should resolve after cache eviction"
       )
      && ok;

  ok = expect(
           resolver.resolve("bare-sized-icon", 32) == bareSizedIcon.string(),
           "index-less inherited theme should resolve icons under 512x512/apps"
       )
      && ok;
  ok = expect(
           resolver.resolve("bare-root-icon", 32) == bareRootIcon.string(),
           "index-less inherited theme should resolve icons at the theme root"
       )
      && ok;

  for (const int size : {0, 32, 64, 128}) {
    ok = expect(
             resolver.resolve("bitmap-priority", size) == (bitmapIconDir / "bitmap-priority.png").string(),
             "current-theme bitmap must precede a better-sized inherited bitmap"
         )
        && ok;
    ok = expect(
             resolver.resolve("theme-priority", size) == themedBitmap.string(),
             "current-theme bitmap must precede an inherited vector icon"
         )
        && ok;
    ok = expect(
             resolver.resolve("later-root", size) == (extraIcons / "later-root.svg").string(),
             "all roots of the current theme must precede inherited themes"
         )
        && ok;
  }

  for (const int size : {0, 48}) {
    ok = expect(
             resolver.resolve("unthemed-app", size) == (root / "icons/unthemed-app.png").string(),
             "an icon at the top of a data dir icons root should resolve"
         )
        && ok;
    ok = expect(
             resolver.resolve("home-unthemed", size) == (root / ".icons/home-unthemed.svg").string(),
             "an icon at the top of ~/.icons should resolve"
         )
        && ok;
    ok = expect(
             resolver.resolve("regular-only", size) == regularOnlyIcon.string(),
             "a themed icon must precede an unthemed icon of the same name"
         )
        && ok;
    ok = expect(
             resolver.resolve("fallback-order", size) == (root / "icons/fallback-order.png").string(),
             "unthemed base dir icons must precede pixmaps"
         )
        && ok;
  }

  setenv("XDG_DATA_DIRS", (extraData.string() + ":" + root.string()).c_str(), 1);
  (void)IconResolver::checkThemePlanChanged();
  ok = expect(
           resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string(),
           "an index-less first root should use metadata from the next theme root"
       )
      && ok;
  setenv("XDG_DATA_DIRS", (root.string() + ":" + extraData.string()).c_str(), 1);
  std::ofstream(extraThemeRoot / "index.theme")
      << "[Icon Theme]\nDirectories = 24x24/status\nInherits = conflicting-parent\n"
         "[24x24/status]\nSize = 24\nType = Fixed\n";
  (void)IconResolver::checkThemePlanChanged();
  ok = expect(
           resolver.resolveStatusVector("split-root", 16) == (extraStatusDir / "split-root.svg").string(),
           "later-root metadata should not replace the first index.theme directory list"
       )
      && ok;

  fs::remove_all(root, ec);
  return ok ? 0 : 1;
}
