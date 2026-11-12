#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class ThemeMode : std::uint8_t;

struct SystemWallpaper {
  std::string name;
  std::filesystem::path light;
  std::filesystem::path dark;

  [[nodiscard]] bool hasDarkVariant() const noexcept { return !dark.empty(); }
  [[nodiscard]] const std::filesystem::path& variant(ThemeMode lightOrDark) const noexcept;
};

class SystemWallpaperCatalog {
public:
  void load(std::span<const std::filesystem::path> dataDirs);

  const std::vector<SystemWallpaper>& wallpapers() const noexcept { return m_wallpapers; }
  const SystemWallpaper* findByVariantPath(std::string_view path) const;
  std::optional<std::filesystem::path> swapForTheme(std::string_view currentPath, ThemeMode lightOrDark) const;

private:
  void add(SystemWallpaper wallpaper);
  void addGnomeBackgroundList(const std::filesystem::path& file);
  void addKdeWallpaperDir(const std::filesystem::path& dir);

  std::vector<SystemWallpaper> m_wallpapers;
  std::unordered_map<std::string, std::size_t> m_indexByRealPath;
};

std::vector<std::filesystem::path> wallpaperDataDirs();
