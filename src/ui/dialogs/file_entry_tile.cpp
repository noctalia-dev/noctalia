#include "ui/dialogs/file_entry_tile.h"
#include "ui/dialogs/file_icon.h"

#include "core/files/directory_scanner.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kPreviewInset = 12.0F;
  constexpr float kPreviewHeightRatio = 0.68F;

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
  setOnLeave([this] {
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

  auto glyph = std::make_unique<FileIcon>();
  glyph->setGlyphSize(36.0F * scale);
  m_glyph = static_cast<FileIcon*>(addChild(std::move(glyph)));

  auto label = ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .maxLines = 1,
      .textAlign = TextAlign::Center,
  });
  m_label = static_cast<Label*>(addChild(std::move(label)));

  auto editor = std::make_unique<Input>();
  m_editor = editor.get();
  m_editor->setControlHeight(28.0F * scale);
  m_editor->setHorizontalPadding(4.0F * scale);
  m_editor->setVisible(false);
  m_editor->setParticipatesInLayout(false);
  addChild(std::move(editor));

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
  if (!m_editPath.empty() && m_editPath != entry.absPath.string()) endInlineEdit(m_editPath);
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

  m_glyph->setFile(entry.absPath, entry.isDir);
  m_label->setText(entry.name);
  m_label->setMaxWidth(std::max(0.0F, width - Style::spaceSm * m_scale * 2.0F));

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
  if (!m_editPath.empty()) endInlineEdit(m_editPath);
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

InputArea* FileEntryTile::beginInlineEdit(const std::string& path, const std::string& name,
    std::function<void(const std::string&)> submit, std::function<void()> cancel) {
  m_editPath = path;
  m_label->setVisible(false);
  m_editor->setVisible(true);
  m_editor->setValue(name);
  m_editor->setOnSubmit(std::move(submit));
  m_editor->setOnKeyEvent([cancel = std::move(cancel)](std::uint32_t sym, std::uint32_t) {
    if (sym != XKB_KEY_Escape) return false;
    cancel();
    return true;
  });
  const auto extension = name.rfind('.');
  if (extension != std::string::npos && extension > 0) m_editor->selectRange(0, extension);
  else m_editor->selectAll();
  markLayoutDirty();
  return m_editor->inputArea();
}

void FileEntryTile::endInlineEdit(const std::string& path) {
  if (m_editPath != path) return;
  m_editPath.clear();
  m_editor->setVisible(false);
  m_label->setVisible(true);
  markLayoutDirty();
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

void FileEntryTile::setSelectionRoles(ColorRole background, ColorRole foreground) {
  m_selectedBackgroundRole = background;
  m_selectedForegroundRole = foreground;
  applyVisualState();
}

void FileEntryTile::doLayout(Renderer& renderer) {
  const float width = this->width();
  const float height = this->height();
  const float previewInset = kPreviewInset * m_scale;
  const float previewWidth = std::max(0.0F, width - previewInset * 2.0F);
  const float previewHeight = std::max(0.0F, height * kPreviewHeightRatio - previewInset);
  const float previewX = previewInset;
  const float previewY = previewInset;
  const float imageInset = Style::spaceSm * m_scale;

  m_background->setPosition(0.0F, 0.0F);
  m_background->setSize(width, height);
  m_preview->setPosition(previewX, previewY);
  m_preview->setSize(previewWidth, previewHeight);

  m_image->setPosition(previewX + imageInset, previewY + imageInset);
  m_image->setSize(std::max(0.0F, previewWidth - imageInset * 2.0F), std::max(0.0F, previewHeight - imageInset * 2.0F));

  if (m_glyph->visible()) {
    m_glyph->measure(renderer);
    m_glyph->setPosition(
        std::round(previewX + (previewWidth - m_glyph->width()) * 0.5F),
        std::round(previewY + (previewHeight - m_glyph->height()) * 0.5F)
    );
  }

  m_label->measure(renderer);
  const float labelY = previewY + previewHeight + Style::spaceSm * m_scale;
  m_label->setPosition(std::round((width - m_label->width()) * 0.5F), labelY);
  m_editor->setPosition(Style::spaceXs * m_scale, labelY - Style::spaceXs * m_scale);
  m_editor->setSize(std::max(0.0F, width - Style::spaceSm * 2.0F * m_scale), 28.0F * m_scale);

  InputArea::doLayout(renderer);
}

void FileEntryTile::applyVisualState() {
  const Color bg = m_selected ? colorForRole(m_selectedBackgroundRole)
      : m_hovered             ? colorForRole(ColorRole::Hover)
                              : clearColor();
  const Color glyphFg = colorForRole(ColorRole::OnSurface); // glyph sits on the preview's Surface bg
  const Color labelFg = m_selected ? colorForRole(m_selectedForegroundRole)
      : m_hovered                  ? colorForRole(ColorRole::OnHover)
                                   : colorForRole(ColorRole::OnSurface);
  const float alpha = m_disabled ? 0.55F : 1.0F;

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
