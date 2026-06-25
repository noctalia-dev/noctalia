#pragma once

#include "render/scene/input_area.h"
#include "shell/wallpaper/panel/wallpaper_scanner.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class Box;
class Button;
class Flex;
class Glyph;
class Image;
class Label;
class Renderer;
class ThumbnailService;
enum class ThemeMode : std::uint8_t;

// A single cell in the wallpaper grid: rounded thumbnail (or folder glyph for
// directories) with filename underneath. Inherits InputArea so the whole tile
// is clickable. Pooled and reused by WallpaperPageGrid across page changes.
class WallpaperTile : public InputArea {
public:
  using ClickCallback = std::function<void(const WallpaperEntry&)>;
  using HoverCallback = std::function<void()>;

  WallpaperTile(float cellWidth, float cellHeight, float contentScale);
  ~WallpaperTile() override;

  // Update the tile's outer cell size and recompute internal frame sizes so
  // the thumbnail/glyph/label adapt to viewport-driven cell dimensions
  // (used by VirtualGridView, which resizes pool tiles on scroll/resize).
  void setCellSize(float cellWidth, float cellHeight);

  // Non-owning pointer to the shared async thumbnail service. If null, tiles
  // will leave their image blank.
  void setThumbnailService(ThumbnailService* service);

  // Bind the tile to an entry. For image entries, a thumbnail is requested
  // from the service; if the previous binding was also an image, its
  // thumbnail is released first so the service only holds textures for the
  // currently bound entries.
  void setEntry(const WallpaperEntry& entry, Renderer& renderer);

  // Detach from the current entry and release any held thumbnail.
  void clearEntry(Renderer& renderer);
  void refreshThumbnail(Renderer& renderer);

  void setSelected(bool selected);
  void setCurrent(bool current);
  void setHoveredVisual(bool hovered);
  void setFavoriteState(bool favorited, std::optional<ThemeMode> themeModeBadge);
  void setStarHovered(bool hovered);
  void setOnStarClick(std::function<void(const WallpaperEntry&)> callback);
  void setOnTileClick(ClickCallback callback);
  void setOnTileMotion(HoverCallback callback);
  void setOnTileEnter(HoverCallback callback);
  void setOnTileLeave(HoverCallback callback);

  [[nodiscard]] static bool
  hitTestStarRegion(float cellWidth, float cellHeight, float contentScale, float localX, float localY) noexcept;

  [[nodiscard]] const WallpaperEntry* entry() const noexcept { return m_hasEntry ? &m_entry : nullptr; }

private:
  void applyVisualState();
  void applyStarVisualState();
  void applyThumbScale(float scale);
  void animateThumbScale(float targetScale);
  void cancelThumbScaleAnimation();
  void layoutThumbOverlays();
  void doLayout(Renderer& renderer) override;
  void releaseThumbnail();
  // Thumbnail long-edge in physical pixels for this tile's current display size.
  [[nodiscard]] int thumbnailTargetPx(const Renderer& renderer) const;

  float m_cellWidth;
  float m_cellHeight;
  float m_contentScale;
  float m_thumbFrameWidth = 0.0f;
  float m_thumbFrameHeight = 0.0f;

  Flex* m_layout = nullptr;
  Box* m_thumbHost = nullptr;
  Image* m_thumb = nullptr;
  Glyph* m_starGlyph = nullptr;
  Button* m_modeBadge = nullptr;
  Glyph* m_folderGlyph = nullptr;
  Glyph* m_loadingGlyph = nullptr;
  Label* m_label = nullptr;

  WallpaperEntry m_entry;
  bool m_hasEntry = false;
  bool m_selected = false;
  bool m_current = false;
  bool m_hoveredVisual = false;
  bool m_favorited = false;
  bool m_starHoveredVisual = false;
  std::optional<ThemeMode> m_themeModeBadge;
  bool m_missingFile = false;
  bool m_loadingThumbnail = false;
  std::string m_thumbPath;
  int m_thumbTargetPx = 0;
  float m_thumbScale = 1.0f;
  float m_thumbScaleTarget = 1.0f;
  std::uint32_t m_thumbScaleAnimId = 0;
  ClickCallback m_onClick;
  std::function<void(const WallpaperEntry&)> m_onStarClick;
  HoverCallback m_onMotion;
  HoverCallback m_onEnter;
  HoverCallback m_onLeave;
  ThumbnailService* m_thumbnails = nullptr;
};
