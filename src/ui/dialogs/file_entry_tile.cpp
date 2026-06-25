#include "ui/dialogs/file_entry_tile.h"

#include "core/files/directory_scanner.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

  constexpr float kPreviewInset = 12.0f;
  constexpr float kPreviewHeightRatio = 0.68f;

} // namespace

FileEntryTile::FileEntryTile(float scale, ThumbnailService* thumbnails) : m_scale(scale), m_thumbnails(thumbnails) {
  setPropagateEvents(true);
  setOnClick([this](const InputArea::PointerData&) {
    if (m_boundIndex != static_cast<std::size_t>(-1) && m_onClick) {
      m_onClick(m_boundIndex);
    }
  });
  setOnMotion([this](const InputArea::PointerData&) {
    if (m_boundIndex != static_cast<std::size_t>(-1) && m_onMotion) {
      m_onMotion(m_boundIndex);
    }
  });
  setOnEnter([this](const InputArea::PointerData&) {
    if (m_boundIndex != static_cast<std::size_t>(-1) && m_onEnter) {
      m_onEnter(m_boundIndex);
    }
  });
  setOnLeave([this]() {
    if (m_boundIndex != static_cast<std::size_t>(-1) && m_onLeave) {
      m_onLeave(m_boundIndex);
    }
  });

  auto background = ui::box({
      .radius = Style::scaledRadiusLg(scale),
  });
  m_background = static_cast<Box*>(addChild(std::move(background)));

  auto preview = ui::box({
      .radius = Style::scaledRadiusMd(scale),
      .cardStyleScale = scale,
  });
  m_preview = static_cast<Box*>(addChild(std::move(preview)));

  auto image = ui::image({
      .fit = ImageFit::Contain,
      .visible = false,
  });
  m_image = static_cast<Image*>(addChild(std::move(image)));

  auto glyph = ui::glyph({
      .glyphSize = 36.0f * scale,
  });
  m_glyph = static_cast<Glyph*>(addChild(std::move(glyph)));

  auto label = ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .maxLines = 1,
      .textAlign = TextAlign::Center,
  });
  m_label = static_cast<Label*>(addChild(std::move(label)));

  setVisible(false);
}

FileEntryTile::~FileEntryTile() { releaseThumbnail(); }

void FileEntryTile::setCallbacks(
    IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave
) {
  m_onClick = std::move(onClick);
  m_onMotion = std::move(onMotion);
  m_onEnter = std::move(onEnter);
  m_onLeave = std::move(onLeave);
}

void FileEntryTile::bind(
    Renderer& renderer, const FileEntry& entry, std::size_t index, float width, float height, bool selected,
    bool hovered, bool disabled
) {
  m_boundIndex = index;
  m_selected = selected;
  m_hovered = hovered;
  m_disabled = disabled;
  setVisible(true);
  setEnabled(true);
  setSize(width, height);
  m_background->setSize(width, height);

  m_thumbnailEligible = !entry.isDir && DirectoryScanner::isImagePath(entry.absPath);
  const std::string nextThumbnailPath = m_thumbnailEligible ? entry.absPath.string() : std::string();
  if (m_thumbnailPath != nextThumbnailPath) {
    releaseThumbnail();
    m_thumbnailPath = nextThumbnailPath;
    if (!m_thumbnailPath.empty() && m_thumbnails != nullptr) {
      (void)m_thumbnails->acquire(m_thumbnailPath);
    }
  }

  m_glyph->setGlyph(entry.isDir ? "folder" : (m_thumbnailEligible ? "image" : "file"));
  m_label->setText(entry.name);
  m_label->setMaxWidth(std::max(0.0f, width - Style::spaceSm * m_scale * 2.0f));

  refreshThumbnail(renderer);
  applyVisualState();
  layout(renderer);
}

void FileEntryTile::refreshThumbnail(Renderer& renderer) {
  if (m_thumbnailEligible && !m_thumbnailPath.empty() && m_thumbnails != nullptr) {
    const TextureHandle handle = m_thumbnails->peek(m_thumbnailPath);
    if (handle.id != 0) {
      m_image->setExternalTexture(renderer, handle);
      m_image->setVisible(true);
      m_glyph->setVisible(false);
      return;
    }
  }

  m_image->clear(renderer);
  m_image->setVisible(false);
  m_glyph->setVisible(true);
}

void FileEntryTile::clear(Renderer& renderer) {
  releaseThumbnail();
  m_image->clear(renderer);
  m_image->setVisible(false);
  m_glyph->setVisible(false);
  m_boundIndex = static_cast<std::size_t>(-1);
  m_selected = false;
  m_hovered = false;
  m_disabled = false;
  m_thumbnailEligible = false;
  setVisible(false);
}

void FileEntryTile::setVisualState(bool selected, bool hovered, bool disabled) {
  if (m_selected == selected && m_hovered == hovered && m_disabled == disabled) {
    return;
  }
  m_selected = selected;
  m_hovered = hovered;
  m_disabled = disabled;
  applyVisualState();
}

void FileEntryTile::doLayout(Renderer& renderer) {
  const float width = this->width();
  const float height = this->height();
  const float previewInset = kPreviewInset * m_scale;
  const float previewWidth = std::max(0.0f, width - previewInset * 2.0f);
  const float previewHeight = std::max(0.0f, height * kPreviewHeightRatio - previewInset);
  const float previewX = previewInset;
  const float previewY = previewInset;
  const float imageInset = Style::spaceSm * m_scale;

  m_background->setPosition(0.0f, 0.0f);
  m_background->setSize(width, height);
  m_preview->setPosition(previewX, previewY);
  m_preview->setSize(previewWidth, previewHeight);

  m_image->setPosition(previewX + imageInset, previewY + imageInset);
  m_image->setSize(std::max(0.0f, previewWidth - imageInset * 2.0f), std::max(0.0f, previewHeight - imageInset * 2.0f));

  if (m_glyph->visible()) {
    m_glyph->measure(renderer);
    m_glyph->setPosition(
        std::round(previewX + (previewWidth - m_glyph->width()) * 0.5f),
        std::round(previewY + (previewHeight - m_glyph->height()) * 0.5f)
    );
  }

  m_label->measure(renderer);
  const float labelY = previewY + previewHeight + Style::spaceSm * m_scale;
  m_label->setPosition(std::round((width - m_label->width()) * 0.5f), labelY);

  InputArea::doLayout(renderer);
}

void FileEntryTile::applyVisualState() {
  const Color bg = m_selected ? colorForRole(ColorRole::Primary)
      : m_hovered             ? colorForRole(ColorRole::Hover)
                              : clearColor();
  const Color glyphFg = colorForRole(ColorRole::OnSurface); // glyph sits on the preview's Surface bg
  const Color labelFg = m_selected ? colorForRole(ColorRole::OnPrimary)
      : m_hovered                  ? colorForRole(ColorRole::OnHover)
                                   : colorForRole(ColorRole::OnSurface);
  const float alpha = m_disabled ? 0.55f : 1.0f;

  m_background->setFill(bg);
  m_glyph->setColor(withAlpha(glyphFg, alpha));
  m_label->setColor(withAlpha(labelFg, alpha));
  markLayoutDirty();
}

void FileEntryTile::releaseThumbnail() {
  if (!m_thumbnailPath.empty() && m_thumbnails != nullptr) {
    m_thumbnails->release(m_thumbnailPath);
  }
  m_thumbnailPath.clear();
}
