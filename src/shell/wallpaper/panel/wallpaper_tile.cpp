#include "shell/wallpaper/panel/wallpaper_tile.h"

#include "config/config_types.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <utility>

namespace {

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

  [[nodiscard]] float overlayControlSize(float contentScale) { return Style::controlHeightSm * contentScale; }

  [[nodiscard]] float starButtonSize(float contentScale) {
    const float starPadding = Style::spaceXs * contentScale;
    return overlayControlSize(contentScale) + starPadding * 2.0f;
  }

  [[nodiscard]] float starShadowOffset(float contentScale) { return std::max(0.5f, 1.0f * contentScale); }

  [[nodiscard]] float activeThumbScale(bool selected, bool current, bool hovered) {
    if (selected || current) {
      return 1.04f;
    }
    if (hovered) {
      return 1.025f;
    }
    return 1.0f;
  }

  void applyStarGlyphStyle(Glyph* glyph, const ColorSpec& fill, float contentScale) {
    if (glyph == nullptr) {
      return;
    }
    glyph->setColor(fill);
    const float offset = starShadowOffset(contentScale);
    glyph->setShadow(Color{0.0f, 0.0f, 0.0f, 0.58f}, 0.0f, offset);
  }

  [[nodiscard]] bool
  starRegionContains(float cellWidth, float /*cellHeight*/, float contentScale, float localX, float localY) {
    const float padding = Style::spaceXs * contentScale;
    const float frameWidth = std::max(0.0f, cellWidth - padding * 2.0f);
    const float inset = Style::spaceXs * contentScale;
    const float btnSize = starButtonSize(contentScale);
    const float starX = padding + frameWidth - btnSize - inset;
    const float starY = padding + inset;
    return localX >= starX && localX < starX + btnSize && localY >= starY && localY < starY + btnSize;
  }

} // namespace

bool WallpaperTile::hitTestStarRegion(
    float cellWidth, float cellHeight, float contentScale, float localX, float localY
) noexcept {
  return starRegionContains(cellWidth, cellHeight, contentScale, localX, localY);
}

WallpaperTile::WallpaperTile(float cellWidth, float cellHeight, float contentScale)
    : m_cellWidth(cellWidth), m_cellHeight(cellHeight), m_contentScale(contentScale) {
  setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  setOnClick([this](const InputArea::PointerData&) {
    if (m_hasEntry && m_onClick) {
      m_onClick(m_entry);
    }
  });
  setOnMotion([this](const InputArea::PointerData&) {
    if (m_hasEntry && m_onMotion) {
      m_onMotion();
    }
  });
  setOnEnter([this](const InputArea::PointerData&) {
    if (m_hasEntry && m_onEnter) {
      m_onEnter();
    }
  });
  setOnLeave([this]() {
    if (m_hasEntry && m_onLeave) {
      m_onLeave();
    }
  });

  const float frameRadius = Style::scaledRadiusLg(m_contentScale);
  const float overlaySize = overlayControlSize(m_contentScale);

  auto layout = ui::column({
      .out = &m_layout,
      .align = FlexAlign::Center,
  });
  addChild(std::move(layout));

  m_layout->addChild(
      ui::box({
          .out = &m_thumbHost,
          .radius = frameRadius,
          .configure = [frameRadius](Box& box) {
            box.setRadius(frameRadius);
            box.setClipChildren(true);
          },
      })
  );

  m_thumbHost->addChild(
      ui::image({
          .out = &m_thumb,
          .fit = ImageFit::Cover,
          .radius = frameRadius,
          .participatesInLayout = false,
      })
  );

  m_thumbHost->addChild(
      ui::glyph({
          .out = &m_folderGlyph,
          .glyph = "folder",
          .color = colorSpecFromRole(ColorRole::Primary),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  m_thumbHost->addChild(
      ui::glyph({
          .out = &m_loadingGlyph,
          .glyph = "hourglass-empty",
          .color = colorSpecFromRole(ColorRole::OnSurface, 0.5f),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  m_thumbHost->addChild(
      ui::glyph({
          .out = &m_starGlyph,
          .glyph = "star",
          .glyphSize = Style::fontSizeBody * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .participatesInLayout = false,
      })
  );

  applyStarVisualState();

  m_thumbHost->addChild(
      ui::button({
          .out = &m_modeBadge,
          .glyph = "sun",
          .glyphSize = Style::fontSizeCaption * m_contentScale,
          .variant = ButtonVariant::Primary,
          .minWidth = overlaySize,
          .minHeight = overlaySize,
          .padding = Style::spaceXs * m_contentScale * 0.5f,
          .radius = Style::scaledRadiusMd(m_contentScale),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  m_layout->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeCaption * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
      })
  );

  setCellSize(cellWidth, cellHeight);
}

WallpaperTile::~WallpaperTile() {
  cancelThumbScaleAnimation();
  releaseThumbnail();
}

void WallpaperTile::setThumbnailService(ThumbnailService* service) {
  if (m_thumbnails == service) {
    return;
  }
  releaseThumbnail();
  m_thumbnails = service;
  if (m_hasEntry && !m_entry.isDir && m_thumbnails != nullptr) {
    m_thumbPath = m_entry.absPath.string();
    // Acquisition is deferred to refreshThumbnail once the display size is known.
  }
}

void WallpaperTile::layoutThumbOverlays() {
  if (m_thumbHost == nullptr || m_thumbFrameWidth <= 0.0f || m_thumbFrameHeight <= 0.0f) {
    return;
  }

  const float inset = Style::spaceXs * m_contentScale;
  const float w = m_thumbFrameWidth;
  const float h = m_thumbFrameHeight;

  if (m_thumb != nullptr) {
    m_thumb->setPosition(0.0f, 0.0f);
    m_thumb->setFrameSize(w, h);
  }

  if (m_starGlyph != nullptr) {
    const float hitSize = starButtonSize(m_contentScale);
    const float glyphW = m_starGlyph->width() > 0.0f ? m_starGlyph->width() : hitSize;
    const float glyphH = m_starGlyph->height() > 0.0f ? m_starGlyph->height() : hitSize;
    m_starGlyph->setPosition(
        std::round(w - hitSize - inset + (hitSize - glyphW) * 0.5f), std::round(inset + (hitSize - glyphH) * 0.5f)
    );
  }

  if (m_modeBadge != nullptr) {
    const float btnW = m_modeBadge->width() > 0.0f ? m_modeBadge->width() : overlayControlSize(m_contentScale);
    const float btnH = m_modeBadge->height() > 0.0f ? m_modeBadge->height() : btnW;
    m_modeBadge->setPosition(std::round(inset), std::round(h - btnH - inset));
    m_modeBadge->setSize(btnW, btnH);
  }

  if (m_folderGlyph != nullptr) {
    const float folderSize = m_folderGlyph->width() > 0.0f ? m_folderGlyph->width() : std::min(w, h) * 0.45f;
    m_folderGlyph->setPosition(std::round((w - folderSize) * 0.5f), std::round((h - folderSize) * 0.5f));
  }

  if (m_loadingGlyph != nullptr) {
    const float loadingSize = m_loadingGlyph->width() > 0.0f ? m_loadingGlyph->width() : std::min(w, h) * 0.32f;
    m_loadingGlyph->setPosition(std::round((w - loadingSize) * 0.5f), std::round((h - loadingSize) * 0.5f));
  }
}

void WallpaperTile::setCellSize(float cellWidth, float cellHeight) {
  m_cellWidth = cellWidth;
  m_cellHeight = cellHeight;
  setSize(cellWidth, cellHeight);

  const float padding = Style::spaceXs * m_contentScale;
  const float innerGap = Style::spaceXs * m_contentScale;
  const float labelH = Style::fontSizeCaption * m_contentScale * 1.4f;
  m_thumbFrameWidth = std::max(0.0f, cellWidth - padding * 2.0f);
  m_thumbFrameHeight = std::max(0.0f, cellHeight - padding * 2.0f - innerGap - labelH);

  if (m_layout != nullptr) {
    m_layout->setGap(innerGap);
    m_layout->setPadding(padding);
    m_layout->setFrameSize(cellWidth, cellHeight);
  }
  if (m_thumbHost != nullptr) {
    m_thumbHost->setFrameSize(m_thumbFrameWidth, m_thumbFrameHeight);
  }
  if (m_folderGlyph != nullptr) {
    m_folderGlyph->setGlyphSize(std::min(m_thumbFrameWidth, m_thumbFrameHeight) * 0.45f);
  }
  if (m_loadingGlyph != nullptr) {
    m_loadingGlyph->setGlyphSize(std::min(m_thumbFrameWidth, m_thumbFrameHeight) * 0.32f);
  }
  if (m_label != nullptr) {
    m_label->setMaxWidth(m_thumbFrameWidth);
  }

  layoutThumbOverlays();
}

void WallpaperTile::doLayout(Renderer& renderer) {
  InputArea::doLayout(renderer);
  if (m_starGlyph != nullptr) {
    m_starGlyph->measure(renderer);
  }
  layoutThumbOverlays();
  if (m_modeBadge != nullptr) {
    m_modeBadge->layout(renderer);
  }
}

void WallpaperTile::setEntry(const WallpaperEntry& entry, Renderer& renderer) {
  const std::string pathString = entry.isDir ? std::string{} : entry.absPath.string();
  const bool sameEntry =
      m_hasEntry && m_entry.absPath == entry.absPath && m_entry.name == entry.name && m_entry.isDir == entry.isDir;
  if (sameEntry) {
    setVisible(true);
    refreshThumbnail(renderer);
    return;
  }

  if (m_thumbPath != pathString) {
    releaseThumbnail();
  }

  m_entry = entry;
  m_hasEntry = true;
  m_missingFile = false;
  setVisible(true);

  m_label->setText(entry.name);

  if (m_starGlyph != nullptr) {
    m_starGlyph->setVisible(!entry.isDir);
  }

  if (entry.isDir) {
    m_thumb->clear(renderer);
    m_thumb->setVisible(false);
    m_loadingThumbnail = false;
    if (m_thumbHost != nullptr) {
      m_thumbHost->setFill(clearColorSpec());
    }
    if (m_folderGlyph != nullptr) {
      m_folderGlyph->setVisible(true);
    }
    if (m_loadingGlyph != nullptr) {
      m_loadingGlyph->setVisible(false);
    }
    if (m_modeBadge != nullptr) {
      m_modeBadge->setVisible(false);
    }
    applyVisualState();
    layoutThumbOverlays();
    return;
  }

  if (m_folderGlyph != nullptr) {
    m_folderGlyph->setVisible(false);
  }
  if (m_loadingGlyph != nullptr) {
    m_loadingGlyph->setVisible(false);
  }

  Color colorFill;
  if (parseColorWallpaperPath(pathString, colorFill)) {
    m_thumbPath.clear();
    m_thumb->clear(renderer);
    m_thumb->setVisible(false);
    m_loadingThumbnail = false;
    if (m_thumbHost != nullptr) {
      m_thumbHost->setFill(fixedColorSpec(colorFill));
    }
    applyVisualState();
    layoutThumbOverlays();
    return;
  }

  if (m_thumbHost != nullptr) {
    m_thumbHost->setFill(clearColorSpec());
  }
  m_thumb->setVisible(true);
  m_thumbPath = pathString;

  std::error_code ec;
  if (!pathString.empty() && !std::filesystem::exists(entry.absPath, ec)) {
    m_missingFile = true;
    m_thumb->clear(renderer);
    m_thumb->setVisible(false);
    if (m_loadingGlyph != nullptr) {
      m_loadingGlyph->setVisible(true);
      m_loadingGlyph->setGlyph("photo-off");
    }
    applyVisualState();
    layoutThumbOverlays();
    return;
  }

  if (m_thumbnails == nullptr) {
    m_thumb->clear(renderer);
    applyVisualState();
    layoutThumbOverlays();
    return;
  }

  refreshThumbnail(renderer);
  applyVisualState();
  layoutThumbOverlays();
}

void WallpaperTile::clearEntry(Renderer& renderer) {
  if (!m_hasEntry && !visible()) {
    return;
  }
  releaseThumbnail();
  if (m_thumb != nullptr) {
    m_thumb->clear(renderer);
  }
  if (m_folderGlyph != nullptr) {
    m_folderGlyph->setVisible(false);
  }
  if (m_loadingGlyph != nullptr) {
    m_loadingGlyph->setVisible(false);
  }
  m_hasEntry = false;
  m_selected = false;
  m_current = false;
  m_hoveredVisual = false;
  m_loadingThumbnail = false;
  applyVisualState();
  setVisible(false);
}

void WallpaperTile::refreshThumbnail(Renderer& renderer) {
  if (!m_hasEntry || m_entry.isDir || m_thumb == nullptr) {
    return;
  }
  if (m_thumbnails == nullptr || m_thumbPath.empty()) {
    m_thumb->clear(renderer);
    return;
  }

  // Decode the thumbnail at the tile's physical display size so it stays crisp
  // under ui_scale / fractional scaling instead of upscaling the 192px default.
  const int targetPx = thumbnailTargetPx(renderer);
  if (targetPx > 0 && targetPx != m_thumbTargetPx) {
    if (m_thumbTargetPx > 0) {
      m_thumbnails->release(m_thumbPath, m_thumbTargetPx);
    }
    (void)m_thumbnails->acquire(m_thumbPath, targetPx);
    m_thumbTargetPx = targetPx;
  }
  if (m_thumbTargetPx <= 0) {
    m_thumb->clear(renderer);
    return;
  }

  const TextureHandle handle = m_thumbnails->peek(m_thumbPath, m_thumbTargetPx);
  if (handle.id != 0) {
    m_loadingThumbnail = false;
    m_thumb->setExternalTexture(renderer, handle);
    m_thumb->setVisible(true);
    if (m_loadingGlyph != nullptr) {
      m_loadingGlyph->setVisible(false);
    }
  } else {
    m_loadingThumbnail = true;
    m_thumb->clear(renderer);
    m_thumb->setVisible(false);
    if (m_loadingGlyph != nullptr) {
      m_loadingGlyph->setVisible(true);
    }
  }
}

void WallpaperTile::releaseThumbnail() {
  if (!m_thumbPath.empty() && m_thumbnails != nullptr && m_thumbTargetPx > 0) {
    m_thumbnails->release(m_thumbPath, m_thumbTargetPx);
  }
  m_thumbPath.clear();
  m_thumbTargetPx = 0;
}

int WallpaperTile::thumbnailTargetPx(const Renderer& renderer) const {
  const float scale = std::max(1.0f, renderer.renderScale());
  const float basis = std::max(m_cellWidth, m_cellHeight);
  if (basis <= 0.0f) {
    return 0;
  }
  return static_cast<int>(std::lround(basis * scale));
}

void WallpaperTile::setSelected(bool selected) {
  if (m_selected == selected) {
    return;
  }
  m_selected = selected;
  applyVisualState();
}

void WallpaperTile::setCurrent(bool current) {
  if (m_current == current) {
    return;
  }
  m_current = current;
  applyVisualState();
}

void WallpaperTile::setOnTileClick(ClickCallback callback) { m_onClick = std::move(callback); }
void WallpaperTile::setOnStarClick(std::function<void(const WallpaperEntry&)> callback) {
  m_onStarClick = std::move(callback);
}

void WallpaperTile::setFavoriteState(bool favorited, std::optional<ThemeMode> themeModeBadge) {
  m_favorited = favorited;
  m_themeModeBadge = themeModeBadge;
  if (m_starGlyph != nullptr) {
    m_starGlyph->setVisible(m_hasEntry && !m_entry.isDir);
  }
  if (m_modeBadge != nullptr) {
    if (themeModeBadge == ThemeMode::Light) {
      m_modeBadge->setGlyph("sun");
      m_modeBadge->setVisible(true);
    } else if (themeModeBadge == ThemeMode::Dark) {
      m_modeBadge->setGlyph("moon");
      m_modeBadge->setVisible(true);
    } else {
      m_modeBadge->setVisible(false);
    }
  }
  applyStarVisualState();
  applyVisualState();
  layoutThumbOverlays();
}
void WallpaperTile::setOnTileMotion(HoverCallback callback) { m_onMotion = std::move(callback); }
void WallpaperTile::setOnTileEnter(HoverCallback callback) { m_onEnter = std::move(callback); }
void WallpaperTile::setOnTileLeave(HoverCallback callback) { m_onLeave = std::move(callback); }

void WallpaperTile::setHoveredVisual(bool hovered) {
  if (m_hoveredVisual == hovered) {
    return;
  }
  m_hoveredVisual = hovered;
  applyVisualState();
}

void WallpaperTile::setStarHovered(bool hovered) {
  if (m_starHoveredVisual == hovered) {
    return;
  }
  m_starHoveredVisual = hovered;
  applyStarVisualState();
}

void WallpaperTile::applyThumbScale(float scale) {
  m_thumbScale = scale;
  if (m_thumbHost != nullptr) {
    m_thumbHost->setScale(scale);
  }
}

void WallpaperTile::animateThumbScale(float targetScale) {
  if (std::abs(m_thumbScaleTarget - targetScale) <= 0.001f) {
    return;
  }

  m_thumbScaleTarget = targetScale;
  if (m_thumbHost == nullptr) {
    m_thumbScale = targetScale;
    return;
  }

  if (m_thumbScaleAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_thumbScaleAnimId);
    m_thumbScaleAnimId = 0;
  }

  if (std::abs(m_thumbScale - targetScale) <= 0.001f || animationManager() == nullptr) {
    applyThumbScale(targetScale);
    return;
  }

  m_thumbScaleAnimId = animationManager()->animate(
      m_thumbScale, targetScale, Style::animFast, Easing::EaseOutCubic, [this](float value) { applyThumbScale(value); },
      [this]() { m_thumbScaleAnimId = 0; }, this
  );
  markPaintDirty();
}

void WallpaperTile::cancelThumbScaleAnimation() {
  if (m_thumbScaleAnimId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_thumbScaleAnimId);
  }
  m_thumbScaleAnimId = 0;
}

void WallpaperTile::applyStarVisualState() {
  if (m_starGlyph == nullptr) {
    return;
  }

  if (m_favorited) {
    m_starGlyph->setGlyph("star-filled");
    applyStarGlyphStyle(m_starGlyph, colorSpecFromRole(ColorRole::Primary), m_contentScale);
  } else if (m_starHoveredVisual) {
    m_starGlyph->setGlyph("star");
    applyStarGlyphStyle(m_starGlyph, colorSpecFromRole(ColorRole::Primary), m_contentScale);
  } else {
    m_starGlyph->setGlyph("star");
    // High-contrast overlay on arbitrary thumbnails: no pill/background, just fill + shadow.
    applyStarGlyphStyle(m_starGlyph, fixedColorSpec(rgba(1.0f, 1.0f, 1.0f, 0.94f)), m_contentScale);
  }
}

void WallpaperTile::applyVisualState() {
  if (m_thumbHost == nullptr || m_thumb == nullptr) {
    return;
  }
  const bool active = m_selected || m_hoveredVisual || m_current;
  setOpacity(m_missingFile ? 0.45f : 1.0f);
  setZIndex((m_selected || m_current) ? 2 : (m_hoveredVisual ? 1 : 0));
  animateThumbScale(activeThumbScale(m_selected, m_current, m_hoveredVisual));
  m_thumb->setTint(active ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.5f, 0.5f, 0.5f, 1.0f));

  auto outlineWidth = Style::borderWidth;
  if (m_selected || m_current || m_hoveredVisual) {
    outlineWidth = Style::emphasizedBorderWidth;
  }

  ColorSpec borderColor = m_selected ? colorSpecFromRole(ColorRole::Primary)
      : m_current                    ? colorSpecFromRole(ColorRole::Secondary)
      : m_hoveredVisual              ? colorSpecFromRole(ColorRole::Hover)
                                     : colorSpecFromRole(ColorRole::Outline);
  ColorSpec labelColor =
      m_current ? colorSpecFromRole(ColorRole::Secondary) : colorSpecFromRole(ColorRole::OnSurfaceVariant);
  ColorSpec frameBg = colorSpecFromRole(ColorRole::SurfaceVariant);
  m_label->setColor(labelColor);

  m_thumbHost->setFill(frameBg);
  if (m_entry.isDir) {
    m_thumbHost->setBorder(borderColor, outlineWidth);
    m_thumb->setBorder(colorSpecFromRole(ColorRole::Outline), outlineWidth);
  } else {
    m_thumbHost->clearBorder();
    m_thumb->setBorder(borderColor, outlineWidth);
  }
}
