#include "system/icon_resolver.h"
#include "test_check.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

  constexpr std::uint16_t kSvgFlag = 1 << 1;
  constexpr std::uint16_t kPngFlag = 1 << 2;
  constexpr std::uint32_t kBucketCount = 3;
  constexpr std::uint32_t kNoEntry = 0xFFFFFFFF;

  struct CachedIcon {
    std::string name;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> images; // directory index, flags
  };

  std::uint32_t gtkNameHash(std::string_view name) {
    std::uint32_t hash = 0;
    for (const char c : name) {
      hash = (hash << 5) - hash + static_cast<std::uint32_t>(static_cast<signed char>(c));
    }
    return hash;
  }

  class CacheWriter {
  public:
    std::string
    build(std::uint16_t majorVersion, const std::vector<std::string>& dirs, const std::vector<CachedIcon>& icons) {
      m_data.clear();
      append16(majorVersion);
      append16(0);
      append32(0);
      append32(0);

      const std::size_t table = m_data.size();
      put32(4, static_cast<std::uint32_t>(table));
      append32(kBucketCount);
      for (std::uint32_t i = 0; i < kBucketCount; ++i) {
        append32(kNoEntry);
      }
      for (const auto& icon : icons) {
        const std::size_t bucket = table + 4 + 4 * (gtkNameHash(icon.name) % kBucketCount);
        const std::size_t entry = m_data.size();
        append32(read32(bucket));
        append32(0);
        append32(0);
        put32(bucket, static_cast<std::uint32_t>(entry));
        put32(entry + 4, appendString(icon.name));
        put32(entry + 8, static_cast<std::uint32_t>(m_data.size()));
        append32(static_cast<std::uint32_t>(icon.images.size()));
        for (const auto& [directory, flags] : icon.images) {
          append16(directory);
          append16(flags);
          append32(0);
        }
      }

      const std::size_t list = m_data.size();
      put32(8, static_cast<std::uint32_t>(list));
      append32(static_cast<std::uint32_t>(dirs.size()));
      for (std::size_t i = 0; i < dirs.size(); ++i) {
        append32(0);
      }
      for (std::size_t i = 0; i < dirs.size(); ++i) {
        put32(list + 4 + 4 * i, appendString(dirs[i]));
      }
      return m_data;
    }

  private:
    void append16(std::uint16_t value) {
      m_data.push_back(static_cast<char>(value >> 8));
      m_data.push_back(static_cast<char>(value));
    }

    void append32(std::uint32_t value) {
      append16(static_cast<std::uint16_t>(value >> 16));
      append16(static_cast<std::uint16_t>(value));
    }

    void put32(std::size_t offset, std::uint32_t value) {
      for (std::size_t i = 0; i < sizeof(value); ++i) {
        m_data[offset + i] = static_cast<char>(value >> (24 - 8 * i));
      }
    }

    std::uint32_t read32(std::size_t offset) const {
      std::uint32_t value = 0;
      for (std::size_t i = 0; i < sizeof(value); ++i) {
        value = value << 8 | static_cast<unsigned char>(m_data[offset + i]);
      }
      return value;
    }

    std::uint32_t appendString(std::string_view text) {
      const auto offset = static_cast<std::uint32_t>(m_data.size());
      m_data.append(text);
      m_data.push_back('\0');
      return offset;
    }

    std::string m_data;
  };

  void writeFile(const fs::path& path, std::string_view contents) { std::ofstream(path, std::ios::binary) << contents; }

} // namespace

int main() {
  char tempDir[] = "/tmp/noctalia-icon-resolver-cache-test-XXXXXX";
  TEST_CHECK(mkdtemp(tempDir) != nullptr);

  const fs::path root(tempDir);
  const fs::path themeRoot = root / "icons/hicolor";
  const fs::path scalableDir = themeRoot / "scalable/apps";
  const fs::path bitmapDir = themeRoot / "48x48/apps";
  const fs::path uncachedDir = themeRoot / "32x32/apps";
  const fs::path cacheFile = themeRoot / "icon-theme.cache";
  fs::create_directories(scalableDir);
  fs::create_directories(bitmapDir);
  fs::create_directories(uncachedDir);
  writeFile(
      themeRoot / "index.theme",
      "[Icon Theme]\nDirectories = scalable/apps, 48x48/apps, 32x32/apps, 64x64/apps\n"
      "[scalable/apps]\nSize = 48\nType = Scalable\n[48x48/apps]\nSize = 48\nType = Fixed\n"
      "[32x32/apps]\nSize = 32\nType = Fixed\n[64x64/apps]\nSize = 64\nType = Fixed\n"
  );
  const fs::path cachedSvg = scalableDir / "cached-app.svg";
  const fs::path cachedPng = bitmapDir / "bitmap-app.png";
  const fs::path unlistedSvg = scalableDir / "unlisted-app.svg";
  writeFile(cachedSvg, "<svg/>");
  writeFile(cachedPng, "png");
  writeFile(unlistedSvg, "<svg/>");
  const fs::path uncachedPng = uncachedDir / "uncached-dir-app.png";
  writeFile(uncachedPng, "png");

  const std::vector<std::string> dirs = {"48x48/apps", "scalable/apps"};
  const std::vector<CachedIcon> icons = {
      {.name = "cached-app", .images = {{1, kSvgFlag}}},
      {.name = "bitmap-app", .images = {{0, kPngFlag}}},
      {.name = "deleted-app", .images = {{1, kSvgFlag}}},
  };
  CacheWriter writer;
  const std::string validCache = writer.build(1, dirs, icons);

  // gtk-update-icon-cache leaves the directories older than the cache file.
  const auto cacheTime = fs::file_time_type::clock::now();
  const auto writeCache = [&](std::string_view contents) {
    fs::remove(cacheFile);
    writeFile(cacheFile, contents);
    fs::last_write_time(cacheFile, cacheTime);
    for (const auto& dir : {themeRoot, scalableDir, bitmapDir, uncachedDir}) {
      fs::last_write_time(dir, cacheTime - std::chrono::hours(1));
    }
  };

  setenv("HOME", tempDir, 1);
  setenv("XDG_DATA_HOME", (root / "empty").c_str(), 1);
  setenv("XDG_DATA_DIRS", root.c_str(), 1);

  writeCache(validCache);
  IconResolver::invalidateThemeContent();
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("cached-app", 48) == cachedSvg.string());
    TEST_CHECK(resolver.resolve("bitmap-app", 48) == cachedPng.string());
    TEST_CHECK(resolver.resolve("deleted-app", 48).empty());
    TEST_CHECK(resolver.resolve("unlisted-app", 48).empty());
    TEST_CHECK(resolver.resolve("uncached-dir-app", 48).empty());
  }

  writeCache(validCache);
  fs::last_write_time(uncachedDir, cacheTime + std::chrono::hours(1));
  IconResolver::invalidateThemeContent();
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("uncached-dir-app", 48) == uncachedPng.string());
  }

  writeCache(validCache);
  fs::last_write_time(scalableDir, cacheTime + std::chrono::hours(1));
  IconResolver::invalidateThemeContent();
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("unlisted-app", 48) == unlistedSvg.string());
    TEST_CHECK(resolver.resolve("bitmap-app", 48) == cachedPng.string());
  }

  const fs::path otherPackage = root / "package/hicolor";
  fs::create_directories(otherPackage);
  writeFile(otherPackage / "icon-theme.cache", validCache);
  writeCache(validCache);
  fs::remove(cacheFile);
  fs::create_symlink(otherPackage / "icon-theme.cache", cacheFile);
  IconResolver::invalidateThemeContent();
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("unlisted-app", 48) == unlistedSvg.string());
  }

  const std::vector<std::pair<std::string, const char*>> badCaches = {
      {writer.build(2, dirs, icons), "a cache with an unknown version should be ignored"},
      {validCache.substr(0, validCache.size() / 2), "a truncated cache should be ignored"},
      {std::string(validCache.size(), '\xFF'), "a corrupt cache should be ignored"},
      {std::string(), "an empty cache should be ignored"},
  };
  for (const auto& [contents, label] : badCaches) {
    writeCache(contents);
    IconResolver::invalidateThemeContent();
    IconResolver resolver(true);
    const bool ignored = resolver.resolve("unlisted-app", 48) == unlistedSvg.string()
        && resolver.resolve("cached-app", 48) == cachedSvg.string();
    if (!ignored) {
      std::println(stderr, "failed: {}", label);
    }
    TEST_CHECK(ignored);
  }

  writeCache(validCache);
  IconResolver::invalidateThemeContent();
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("unlisted-app", 48).empty());
    writeCache(writer.build(1, dirs, {{.name = "unlisted-app", .images = {{1, kSvgFlag}}}}));
    resolver.invalidateMissingCache();
    TEST_CHECK(resolver.resolve("unlisted-app", 48) == unlistedSvg.string());
  }

  // 64x64/apps is listed in index.theme but only created now, after the root was read.
  {
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("late-dir-app", 48).empty());
    const fs::path lateDir = themeRoot / "64x64/apps";
    fs::create_directories(lateDir);
    writeFile(lateDir / "late-dir-app.png", "png");
    resolver.invalidateMissingCache();
    TEST_CHECK(resolver.resolve("late-dir-app", 48) == (lateDir / "late-dir-app.png").string());
  }

  // A theme without index.theme is searched at its root, which GTK's cache doesn't list.
  {
    const fs::path bareRoot = root / "icons/bare";
    fs::create_directories(bareRoot);
    fs::create_directories(root / ".config/gtk-3.0");
    writeFile(root / ".config/gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=bare\n");
    writeFile(bareRoot / "root-app.png", "png");
    writeFile(bareRoot / "icon-theme.cache", writer.build(1, {"48x48/apps"}, {}));
    fs::last_write_time(bareRoot / "icon-theme.cache", cacheTime);
    fs::last_write_time(bareRoot, cacheTime - std::chrono::hours(1));
    IconResolver::invalidateThemeContent();
    IconResolver resolver(true);
    TEST_CHECK(resolver.resolve("root-app", 48) == (bareRoot / "root-app.png").string());
  }

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
