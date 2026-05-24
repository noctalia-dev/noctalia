#include "shell/wallpaper/panel/wallpaper_tile.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

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
  const float outlineWidth = Style::borderWidth * 2.0f;

  auto layout = ui::column({
      .out = &m_layout,
      .align = FlexAlign::Center,
  });
  addChild(std::move(layout));

  m_layout->addChild(
      ui::column({
          .out = &m_thumbBox,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .configure = [frameRadius](Flex& flex) { flex.setRadius(frameRadius); },
      })
  );

  m_thumbBox->addChild(
      ui::image({
          .out = &m_thumb,
          .fit = ImageFit::Cover,
          .radius = frameRadius,
          .configure = [outlineWidth](Image& image) {
            image.setBorder(colorSpecFromRole(ColorRole::Outline), outlineWidth);
          },
      })
  );

  m_thumbBox->addChild(
      ui::glyph({
          .out = &m_folderGlyph,
          .glyph = "folder",
          .color = colorSpecFromRole(ColorRole::Primary),
          .visible = false,
      })
  );

  m_thumbBox->addChild(
      ui::glyph({
          .out = &m_loadingGlyph,
          .glyph = "hourglass-empty",
          .color = colorSpecFromRole(ColorRole::OnSurface, 0.5f),
          .visible = false,
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

WallpaperTile::~WallpaperTile() { releaseThumbnail(); }

void WallpaperTile::setThumbnailService(ThumbnailService* service) {
  if (m_thumbnails == service) {
    return;
  }
  releaseThumbnail();
  m_thumbnails = service;
  if (m_hasEntry && !m_entry.isDir && m_thumbnails != nullptr) {
    m_thumbPath = m_entry.absPath.string();
    if (!m_thumbPath.empty()) {
      (void)m_thumbnails->acquire(m_thumbPath);
    }
  }
}

void WallpaperTile::setCellSize(float cellWidth, float cellHeight) {
  m_cellWidth = cellWidth;
  m_cellHeight = cellHeight;
  setSize(cellWidth, cellHeight);

  const float padding = Style::spaceXs * m_contentScale;
  const float innerGap = Style::spaceXs * m_contentScale;
  const float labelH = Style::fontSizeCaption * m_contentScale * 1.4f;
  const float frameWidth = std::max(0.0f, cellWidth - padding * 2.0f);
  const float frameHeight = std::max(0.0f, cellHeight - padding * 2.0f - innerGap - labelH);

  if (m_layout != nullptr) {
    m_layout->setGap(innerGap);
    m_layout->setPadding(padding);
    m_layout->setFrameSize(cellWidth, cellHeight);
  }
  if (m_thumbBox != nullptr) {
    m_thumbBox->setMinWidth(frameWidth);
    m_thumbBox->setMinHeight(frameHeight);
    m_thumbBox->setFrameSize(frameWidth, frameHeight);
  }
  if (m_thumb != nullptr) {
    m_thumb->setFrameSize(frameWidth, frameHeight);
  }
  if (m_folderGlyph != nullptr) {
    m_folderGlyph->setGlyphSize(std::min(frameWidth, frameHeight) * 0.45f);
  }
  if (m_loadingGlyph != nullptr) {
    m_loadingGlyph->setGlyphSize(std::min(frameWidth, frameHeight) * 0.32f);
  }
  if (m_label != nullptr) {
    m_label->setMaxWidth(frameWidth);
  }
}

void WallpaperTile::doLayout(Renderer& renderer) { InputArea::doLayout(renderer); }

void WallpaperTile::setEntry(const WallpaperEntry& entry, Renderer& renderer) {
  const std::string newPath = entry.isDir ? std::string{} : entry.absPath.string();
  const bool sameEntry =
      m_hasEntry && m_entry.absPath == entry.absPath && m_entry.name == entry.name && m_entry.isDir == entry.isDir;
  if (sameEntry) {
    setVisible(true);
    refreshThumbnail(renderer);
    return;
  }

  if (m_thumbPath != newPath) {
    releaseThumbnail();
  }

  m_entry = entry;
  m_hasEntry = true;
  setVisible(true);

  m_label->setText(entry.name);

  if (entry.isDir) {
    m_thumb->clear(renderer);
    m_thumb->setVisible(false);
    m_loadingThumbnail = false;
    if (m_folderGlyph != nullptr) {
      m_folderGlyph->setVisible(true);
    }
    if (m_loadingGlyph != nullptr) {
      m_loadingGlyph->setVisible(false);
    }
    applyVisualState();
    return;
  }

  if (m_folderGlyph != nullptr) {
    m_folderGlyph->setVisible(false);
  }
  if (m_loadingGlyph != nullptr) {
    m_loadingGlyph->setVisible(false);
  }
  m_thumb->setVisible(true);
  m_thumbPath = newPath;

  if (m_thumbnails == nullptr) {
    m_thumb->clear(renderer);
    applyVisualState();
    return;
  }

  if (!m_thumbPath.empty()) {
    (void)m_thumbnails->acquire(m_thumbPath);
  }
  refreshThumbnail(renderer);
  applyVisualState();
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

  const TextureHandle handle = m_thumbnails->peek(m_thumbPath);
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
  if (!m_thumbPath.empty() && m_thumbnails != nullptr) {
    m_thumbnails->release(m_thumbPath);
  }
  m_thumbPath.clear();
}

void WallpaperTile::setSelected(bool selected) {
  if (m_selected == selected) {
    return;
  }
  m_selected = selected;
  applyVisualState();
}

void WallpaperTile::setOnTileClick(ClickCallback callback) { m_onClick = std::move(callback); }
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

void WallpaperTile::applyVisualState() {
  if (m_thumbBox == nullptr || m_thumb == nullptr) {
    return;
  }
  const bool active = m_selected || m_hoveredVisual;
  setOpacity(1.0f);
  m_thumb->setTint(active ? rgba(1.0f, 1.0f, 1.0f, 1.0f) : rgba(0.5f, 0.5f, 0.5f, 1.0f));

  const float outlineWidth = Style::borderWidth * 3.0f;
  ColorSpec borderColor = active ? colorSpecFromRole(ColorRole::Hover) : colorSpecFromRole(ColorRole::Outline);
  ColorSpec frameBg = colorSpecFromRole(ColorRole::SurfaceVariant);

  m_thumbBox->setFill(frameBg);
  if (m_entry.isDir) {
    // Folder tiles hide the image node, so draw the state outline on the frame.
    m_thumbBox->setBorder(borderColor, outlineWidth);
    m_thumb->setBorder(colorSpecFromRole(ColorRole::Outline), outlineWidth);
  } else {
    m_thumbBox->clearBorder();
    m_thumb->setBorder(borderColor, outlineWidth);
  }
}
