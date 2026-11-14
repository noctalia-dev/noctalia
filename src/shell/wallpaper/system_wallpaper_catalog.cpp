#include "shell/wallpaper/system_wallpaper_catalog.h"

#include "config/config_types.h"
#include "core/log.h"
#include "system/directory_scanner.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <libxml/parser.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

  constexpr Logger kLog("wallpaper");

  std::string realPathKey(const std::filesystem::path& path) {
    std::error_code ec;
    const auto real = std::filesystem::weakly_canonical(path, ec);
    return (ec ? path.lexically_normal() : real).string();
  }

  bool isImageFile(const std::filesystem::path& path) {
    std::error_code ec;
    return DirectoryScanner::isImagePath(path) && std::filesystem::is_regular_file(path, ec) && !ec;
  }

  std::vector<std::filesystem::path> sortedChildren(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> children;
    for (std::error_code ec; const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      children.push_back(entry.path());
    }
    std::ranges::sort(children);
    return children;
  }

  void appendSearchPath(
      std::vector<std::filesystem::path>& dirs, std::unordered_set<std::string>& seen, std::string_view searchPath
  ) {
    std::size_t start = 0;
    while (start <= searchPath.size()) {
      const std::size_t end = std::min(searchPath.find(':', start), searchPath.size());
      if (const std::string_view dir = searchPath.substr(start, end - start);
          dir.starts_with('/') && seen.emplace(dir).second) {
        dirs.emplace_back(dir);
      }
      start = end + 1;
    }
  }

  using XmlDoc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

  bool hasName(const xmlNode* node, const std::string_view name) {
    return node->type == XML_ELEMENT_NODE && reinterpret_cast<const char*>(node->name) == name;
  }

  std::string nodeText(const xmlNode* node) {
    xmlChar* content = xmlNodeGetContent(node);
    if (content == nullptr) {
      return {};
    }
    const std::string text(reinterpret_cast<const char*>(content));
    xmlFree(content);
    const auto first = text.find_first_not_of(" \t\r\n");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
  }

  bool isLocalized(const xmlNode* node) {
    xmlChar* lang = xmlGetNsProp(node, reinterpret_cast<const xmlChar*>("lang"), XML_XML_NAMESPACE);
    if (lang == nullptr) {
      return false;
    }
    xmlFree(lang);
    return true;
  }

  bool isMarkedDeleted(const xmlNode* node) {
    xmlChar* deleted = xmlGetProp(node, reinterpret_cast<const xmlChar*>("deleted"));
    if (deleted == nullptr) {
      return false;
    }
    const bool marked = std::string_view(reinterpret_cast<const char*>(deleted)) == "true";
    xmlFree(deleted);
    return marked;
  }

  std::filesystem::path gnomeImagePath(const std::string& value, const std::filesystem::path& listDir) {
    if (value.empty() || value == "(none)") {
      return {};
    }
    const std::filesystem::path path(value);
    return path.is_absolute() ? path : listDir / path;
  }

  struct KdeImageSize {
    std::uint64_t width = 0;
    std::uint64_t height = 0;

    [[nodiscard]] std::uint64_t pixels() const noexcept { return width * height; }

    [[nodiscard]] bool fitsDesktopScreen() const noexcept {
      constexpr KdeImageSize kNarrowest{.width = 4, .height = 3};
      constexpr KdeImageSize kWidest{.width = 21, .height = 9};
      return width * kNarrowest.height >= height * kNarrowest.width && width * kWidest.height <= height * kWidest.width;
    }
  };

  KdeImageSize kdeImageSizeFromStem(const std::string& stem) {
    const auto separator = stem.find('x');
    if (separator == std::string::npos) {
      return {};
    }
    KdeImageSize size;
    const char* begin = stem.data();
    if (const char* end = stem.data() + stem.size();
        std::from_chars(begin, begin + separator, size.width).ec != std::errc{}
        || std::from_chars(begin + separator + 1, end, size.height).ec != std::errc{}) {
      return {};
    }
    return size;
  }

  std::filesystem::path bestKdeImage(const std::filesystem::path& imagesDir) {
    std::filesystem::path best;
    KdeImageSize bestSize;
    const auto preferable = [](const KdeImageSize& candidate, const KdeImageSize& current) {
      if (candidate.fitsDesktopScreen() != current.fitsDesktopScreen()) {
        return candidate.fitsDesktopScreen();
      }
      return candidate.pixels() > current.pixels();
    };
    for (const auto& path : sortedChildren(imagesDir)) {
      if (!isImageFile(path)) {
        continue;
      }
      if (const KdeImageSize size = kdeImageSizeFromStem(path.stem().string());
          best.empty() || preferable(size, bestSize)) {
        best = path;
        bestSize = size;
      }
    }
    return best;
  }

  std::string kdePackageName(const std::filesystem::path& packageDir) {
    if (std::ifstream metadata(packageDir / "metadata.json"); metadata) {
      if (const nlohmann::json root = nlohmann::json::parse(metadata, nullptr, false); !root.is_discarded()) {
        if (const auto plugin = root.find("KPlugin"); plugin != root.end() && plugin->is_object()) {
          if (const auto name = plugin->find("Name");
              name != plugin->end() && name->is_string() && !name->get<std::string>().empty()) {
            return name->get<std::string>();
          }
        }
      }
    }
    return packageDir.filename().string();
  }

} // namespace

const std::filesystem::path& SystemWallpaper::variant(const ThemeMode lightOrDark) const noexcept {
  return lightOrDark == ThemeMode::Dark && hasDarkVariant() ? dark : light;
}

void SystemWallpaperCatalog::load(const std::span<const std::filesystem::path> dataDirs) {
  m_wallpapers.clear();
  m_indexByRealPath.clear();

  for (const auto& dataDir : dataDirs) {
    for (const auto& file : sortedChildren(dataDir / "gnome-background-properties")) {
      if (file.extension() == ".xml") {
        addGnomeBackgroundList(file);
      }
    }
  }
  for (const auto& dataDir : dataDirs) {
    addKdeWallpaperDir(dataDir / "wallpapers");
  }

  kLog.debug("found {} system wallpapers", m_wallpapers.size());
}

const SystemWallpaper* SystemWallpaperCatalog::findByVariantPath(const std::string_view path) const {
  if (path.empty() || path.starts_with("color:")) {
    return nullptr;
  }
  const auto it = m_indexByRealPath.find(realPathKey(std::filesystem::path(path)));
  return it == m_indexByRealPath.end() ? nullptr : &m_wallpapers[it->second];
}

std::optional<std::filesystem::path>
SystemWallpaperCatalog::swapForTheme(const std::string_view currentPath, const ThemeMode lightOrDark) const {
  const SystemWallpaper* wallpaper = findByVariantPath(currentPath);
  if (wallpaper == nullptr) {
    return std::nullopt;
  }
  const std::filesystem::path& wanted = wallpaper->variant(lightOrDark);
  if (realPathKey(wanted) == realPathKey(std::filesystem::path(currentPath))) {
    return std::nullopt;
  }
  return wanted;
}

void SystemWallpaperCatalog::add(SystemWallpaper wallpaper) {
  if (!isImageFile(wallpaper.light)) {
    return;
  }
  if (wallpaper.hasDarkVariant() && !isImageFile(wallpaper.dark)) {
    wallpaper.dark.clear();
  }

  const std::size_t index = m_wallpapers.size();
  if (!m_indexByRealPath.emplace(realPathKey(wallpaper.light), index).second) {
    return;
  }
  if (wallpaper.hasDarkVariant()) {
    m_indexByRealPath.emplace(realPathKey(wallpaper.dark), index);
  }
  if (wallpaper.name.empty()) {
    wallpaper.name = wallpaper.light.stem().string();
  }
  m_wallpapers.push_back(std::move(wallpaper));
}

void SystemWallpaperCatalog::addGnomeBackgroundList(const std::filesystem::path& file) {
  const XmlDoc doc(
      xmlReadFile(file.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), &xmlFreeDoc
  );
  const xmlNode* root = doc != nullptr ? xmlDocGetRootElement(doc.get()) : nullptr;
  if (root == nullptr) {
    kLog.warn("skipping unreadable wallpaper list {}", file.string());
    return;
  }

  const std::filesystem::path listDir = file.parent_path();
  for (const xmlNode* node = root->children; node != nullptr; node = node->next) {
    if (!hasName(node, "wallpaper") || isMarkedDeleted(node)) {
      continue;
    }

    SystemWallpaper wallpaper;
    for (const xmlNode* field = node->children; field != nullptr; field = field->next) {
      if (hasName(field, "name") && !isLocalized(field)) {
        wallpaper.name = nodeText(field);
      } else if (hasName(field, "filename")) {
        wallpaper.light = gnomeImagePath(nodeText(field), listDir);
      } else if (hasName(field, "filename-dark")) {
        wallpaper.dark = gnomeImagePath(nodeText(field), listDir);
      }
    }
    add(std::move(wallpaper));
  }
}

void SystemWallpaperCatalog::addKdeWallpaperDir(const std::filesystem::path& dir) {
  for (const auto& path : sortedChildren(dir)) {
    if (std::error_code ec; std::filesystem::is_directory(path, ec) && !ec) {
      add({
          .name = kdePackageName(path),
          .light = bestKdeImage(path / "contents" / "images"),
          .dark = bestKdeImage(path / "contents" / "images_dark"),
      });
    } else if (isImageFile(path)) {
      add({.name = path.stem().string(), .light = path, .dark = {}});
    }
  }
}

std::vector<std::filesystem::path> wallpaperDataDirs() {
  std::vector<std::filesystem::path> dirs;
  std::unordered_set<std::string> seen;

  if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && dataHome[0] != '\0') {
    appendSearchPath(dirs, seen, dataHome);
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    appendSearchPath(dirs, seen, std::string(home) + "/.local/share");
  }

  const char* dataDirs = std::getenv("XDG_DATA_DIRS");
  appendSearchPath(dirs, seen, dataDirs != nullptr && dataDirs[0] != '\0' ? dataDirs : "/usr/local/share:/usr/share");
  return dirs;
}
