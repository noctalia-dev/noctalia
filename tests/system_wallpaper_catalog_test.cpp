#include "config/config_types.h"
#include "shell/wallpaper/system_wallpaper_catalog.h"
#include "tests/test_check.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

  void writeFile(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  void touch(const std::filesystem::path& path) { writeFile(path, "image"); }

  void symlinkTo(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(target, link);
  }

  const SystemWallpaper* byName(const SystemWallpaperCatalog& catalog, const std::string_view name) {
    for (const auto& wallpaper : catalog.wallpapers()) {
      if (wallpaper.name == name) {
        return &wallpaper;
      }
    }
    return nullptr;
  }

  void buildFixture(const std::filesystem::path& share) {
    const auto images = share / "backgrounds" / "distro";
    touch(images / "pair-light.png");
    touch(images / "pair-dark.png");
    touch(images / "single.png");
    touch(images / "relative.jpg");
    touch(images / "localized.png");
    touch(images / "deleted.png");
    touch(images / "slideshow.xml");

    writeFile(
        share / "gnome-background-properties" / "distro.xml",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE wallpapers SYSTEM "gnome-wp-list.dtd">
<wallpapers>
  <wallpaper deleted="false">
    <name>Pair</name>
    <filename>)"
            + (images / "pair-light.png").string()
            + R"(</filename>
    <filename-dark>)"
            + (images / "pair-dark.png").string()
            + R"(</filename-dark>
  </wallpaper>
  <wallpaper>
    <name>Single</name>
    <filename>)"
            + (images / "single.png").string()
            + R"(</filename>
    <filename-dark>)"
            + (images / "missing-dark.png").string()
            + R"(</filename-dark>
  </wallpaper>
  <wallpaper>
    <name>Relative</name>
    <filename>../backgrounds/distro/relative.jpg</filename>
  </wallpaper>
  <wallpaper>
    <name xml:lang="de">Lokalisiert</name>
    <name>Localized</name>
    <filename>)"
            + (images / "localized.png").string()
            + R"(</filename>
  </wallpaper>
  <wallpaper deleted="true">
    <name>Deleted</name>
    <filename>)"
            + (images / "deleted.png").string()
            + R"(</filename>
  </wallpaper>
  <wallpaper>
    <name>Slideshow</name>
    <filename>)"
            + (images / "slideshow.xml").string()
            + R"(</filename>
  </wallpaper>
  <wallpaper>
    <name>Missing</name>
    <filename>)"
            + (images / "missing.png").string()
            + R"(</filename>
  </wallpaper>
</wallpapers>
)"
    );
    writeFile(share / "gnome-background-properties" / "broken.xml", "<wallpapers><wallpaper>");

    const auto kdeDuplicate = share / "wallpapers" / "PairPackage" / "contents";
    symlinkTo(images / "pair-light.png", kdeDuplicate / "images" / "3840x2160.png");
    symlinkTo(images / "pair-dark.png", kdeDuplicate / "images_dark" / "3840x2160.png");

    const auto kdePackage = share / "wallpapers" / "Mountains";
    writeFile(kdePackage / "metadata.json", R"({"KPlugin": {"Id": "Mountains", "Name": "Big Mountains"}})");
    touch(kdePackage / "contents" / "images" / "1920x1080.png");
    touch(kdePackage / "contents" / "images" / "3840x2160.png");
    touch(kdePackage / "contents" / "images" / "1280x720.png");
    touch(kdePackage / "contents" / "images" / "7680x2160.png");
    touch(kdePackage / "contents" / "images" / "1440x2960.png");
    touch(kdePackage / "contents" / "images_dark" / "1920x1080.png");

    touch(share / "wallpapers" / "Loose.webp");
    touch(share / "wallpapers" / "notes.txt");
  }

  void checkCatalog(const std::filesystem::path& share) {
    const std::vector dataDirs = {share};
    SystemWallpaperCatalog catalog;
    catalog.load(dataDirs);

    TEST_CHECK(catalog.wallpapers().size() == 6);

    const auto images = share / "backgrounds" / "distro";
    const SystemWallpaper* pair = byName(catalog, "Pair");
    TEST_CHECK(pair != nullptr);
    if (pair != nullptr) {
      TEST_CHECK(pair->light == images / "pair-light.png");
      TEST_CHECK(pair->dark == images / "pair-dark.png");
      TEST_CHECK(pair->variant(ThemeMode::Dark) == images / "pair-dark.png");
      TEST_CHECK(pair->variant(ThemeMode::Light) == images / "pair-light.png");
    }
    TEST_CHECK(byName(catalog, "PairPackage") == nullptr);

    const SystemWallpaper* single = byName(catalog, "Single");
    TEST_CHECK(single != nullptr && !single->hasDarkVariant());
    if (single != nullptr) {
      TEST_CHECK(single->variant(ThemeMode::Dark) == images / "single.png");
    }

    TEST_CHECK(byName(catalog, "Relative") != nullptr);
    TEST_CHECK(byName(catalog, "Localized") != nullptr);
    TEST_CHECK(byName(catalog, "Lokalisiert") == nullptr);
    TEST_CHECK(byName(catalog, "Deleted") == nullptr);
    TEST_CHECK(byName(catalog, "Slideshow") == nullptr);
    TEST_CHECK(byName(catalog, "Missing") == nullptr);

    const SystemWallpaper* mountains = byName(catalog, "Big Mountains");
    TEST_CHECK(mountains != nullptr);
    if (mountains != nullptr) {
      TEST_CHECK(mountains->light.filename() == "3840x2160.png");
      TEST_CHECK(mountains->dark.filename() == "1920x1080.png");
    }
    TEST_CHECK(byName(catalog, "Loose") != nullptr);
  }

  void checkSwapForTheme(const std::filesystem::path& share) {
    const std::vector<std::filesystem::path> dataDirs = {share};
    SystemWallpaperCatalog catalog;
    catalog.load(dataDirs);

    const auto images = share / "backgrounds" / "distro";
    const std::string light = (images / "pair-light.png").string();
    const std::string dark = (images / "pair-dark.png").string();
    const std::string lightThroughSymlink =
        (share / "wallpapers" / "PairPackage" / "contents" / "images" / "3840x2160.png").string();

    TEST_CHECK(catalog.swapForTheme(light, ThemeMode::Dark) == std::filesystem::path(dark));
    TEST_CHECK(catalog.swapForTheme(dark, ThemeMode::Light) == std::filesystem::path(light));
    TEST_CHECK(!catalog.swapForTheme(light, ThemeMode::Light).has_value());
    TEST_CHECK(!catalog.swapForTheme(lightThroughSymlink, ThemeMode::Light).has_value());
    TEST_CHECK(catalog.swapForTheme(lightThroughSymlink, ThemeMode::Dark) == std::filesystem::path(dark));
    TEST_CHECK(!catalog.swapForTheme((images / "single.png").string(), ThemeMode::Dark).has_value());
    TEST_CHECK(!catalog.swapForTheme("/elsewhere/user.png", ThemeMode::Dark).has_value());
    TEST_CHECK(!catalog.swapForTheme("color:#112233", ThemeMode::Dark).has_value());
  }

  void checkFirstDataDirWins(const std::filesystem::path& root) {
    const auto user = root / "user-share";
    const auto system = root / "system-share";
    touch(system / "backgrounds" / "shared.png");
    writeFile(
        user / "gnome-background-properties" / "override.xml",
        "<wallpapers><wallpaper><name>User Name</name><filename>"
            + (system / "backgrounds" / "shared.png").string()
            + "</filename></wallpaper></wallpapers>"
    );
    writeFile(
        system / "gnome-background-properties" / "vendor.xml",
        "<wallpapers><wallpaper><name>Vendor Name</name><filename>"
            + (system / "backgrounds" / "shared.png").string()
            + "</filename></wallpaper></wallpapers>"
    );

    const std::vector<std::filesystem::path> dataDirs = {user, system};
    SystemWallpaperCatalog catalog;
    catalog.load(dataDirs);
    TEST_CHECK(catalog.wallpapers().size() == 1);
    TEST_CHECK(byName(catalog, "User Name") != nullptr);
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-system-wallpapers-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto share = root / "share";
  buildFixture(share);

  checkCatalog(share);
  checkSwapForTheme(share);
  checkFirstDataDirWins(root);

  std::filesystem::remove_all(root);
  return 0;
}
