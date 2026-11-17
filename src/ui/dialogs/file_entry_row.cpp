#include "ui/dialogs/file_entry_row.h"

#include "core/files/directory_scanner.h"
#include "filemanager/file_columns.h"
#include "filemanager/file_interaction.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/core/thumbnail_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr float kSizeColumnWidth = 96.0F;
  constexpr float kDateColumnWidth = 152.0F;

  std::string formatSize(const FileEntry& entry) {
    if (entry.isDir) {
      return i18n::tr("ui.dialogs.file.entry.folder");
    }

    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    auto size = static_cast<double>(entry.size);
    std::size_t unit = 0;
    while (size >= 1024.0 && unit + 1 < std::size(kUnits)) {
      size /= 1024.0;
      ++unit;
    }

    std::ostringstream out;
    if (unit == 0) {
      out << static_cast<std::uintmax_t>(std::llround(size)) << ' ' << kUnits[unit];
    } else {
      out.setf(std::ios::fixed, std::ios::floatfield);
      out.precision(size >= 10.0 ? 0 : 1);
      out << size << ' ' << kUnits[unit];
    }
    return out.str();
  }

} // namespace

FileEntryRow::FileEntryRow(float scale, float iconSize, ThumbnailService* thumbnails)
    : m_scale(scale),
      m_rowHeight(std::max(32.0F * scale, (iconSize > 0.0F ? iconSize : 22.0F * scale) + 10.0F * scale)),
      m_iconSize(iconSize > 0.0F ? iconSize : 22.0F * scale), m_thumbnails(thumbnails) {
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
      .radius = Style::scaledRadiusMd(scale),
  });
  m_background = static_cast<Box*>(addChild(std::move(background)));

  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& container) { container.setPadding(Style::spaceXs * scale, Style::spaceSm * scale); },
  });
  m_row = static_cast<Flex*>(addChild(std::move(row)));

  auto icon = std::make_unique<FileIcon>();
  icon->setGlyphSize(m_iconSize);
  m_icon = static_cast<FileIcon*>(m_row->addChild(std::move(icon)));
  auto image = ui::image({.fit = ImageFit::Contain, .visible = false});
  m_image = static_cast<Image*>(m_row->addChild(std::move(image)));
  m_image->setParticipatesInLayout(false);

  auto name = ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .flexGrow = 1.0F,
  });
  m_name = static_cast<Label*>(m_row->addChild(std::move(name)));
  // The row owns fixed column widths. Leaving the Name label flexible lets Flex
  // absorb spare row space and pushes Size/Modified away from their headers.
  m_name->setFlexGrow(0.0F);

  auto editor = std::make_unique<Input>();
  m_editor = editor.get();
  m_editor->setControlHeight(28.0F * scale);
  m_editor->setHorizontalPadding(4.0F * scale);
  m_editor->setVisible(false);
  m_editor->setParticipatesInLayout(false);
  m_editor->setFlexGrow(1.0F);
  m_row->addChild(std::move(editor));

  auto size = ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .minWidth = kSizeColumnWidth * scale,
      .textAlign = TextAlign::End,
  });
  m_size = static_cast<Label*>(m_row->addChild(std::move(size)));

  auto date = ui::label({
      .fontSize = Style::fontSizeCaption * scale,
      .minWidth = kDateColumnWidth * scale,
      .textAlign = TextAlign::End,
  });
  m_date = static_cast<Label*>(m_row->addChild(std::move(date)));

  setVisible(false);
}

FileEntryRow::~FileEntryRow() { releaseThumbnail(); }

void FileEntryRow::releaseThumbnail() {
  if (m_thumbnails != nullptr && !m_thumbnailPath.empty())
    m_thumbnails->release(m_thumbnailPath, 64);
  m_thumbnailPath.clear();
}

void FileEntryRow::refreshThumbnail(Renderer& renderer) {
  if (!m_thumbnailPath.empty() && m_thumbnails != nullptr) {
    const auto handle = m_thumbnails->peek(m_thumbnailPath, 64);
    if (handle.id != 0) {
      m_image->setExternalTexture(renderer, handle);
      m_image->setVisible(true);
      m_icon->setVisible(false);
      return;
    }
  }
  m_image->clear(renderer);
  m_image->setVisible(false);
  m_icon->setVisible(true);
}

void FileEntryRow::setCallbacks(
    IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave
) {
  m_onClick = std::move(onClick);
  m_onMotion = std::move(onMotion);
  m_onEnter = std::move(onEnter);
  m_onLeave = std::move(onLeave);
}

void FileEntryRow::setColumnWidths(float name, float size, float modified) {
  m_nameColumnWidth = std::max(0.0F, name);
  m_sizeColumnWidth = std::max(0.0F, size);
  m_dateColumnWidth = std::max(0.0F, modified);
  const float sizeTextInset = filemanager::kModifiedResizeMarkerInset - Style::spaceSm * 0.5F;
  const float sizeTextWidth = std::max(0.0F, m_sizeColumnWidth - sizeTextInset) * m_scale;
  m_size->setMinWidth(sizeTextWidth);
  m_size->setMaxWidth(sizeTextWidth);
  const float dateTextWidth = std::max(0.0F, m_dateColumnWidth - filemanager::kModifiedResizeMarkerInset) * m_scale;
  m_date->setMinWidth(dateTextWidth);
  m_date->setMaxWidth(dateTextWidth);
}

void FileEntryRow::bind(
    Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
    bool disabled
) {
  m_boundIndex = index;
  if (!m_editPath.empty() && m_editPath != entry.absPath.string())
    endInlineEdit(m_editPath);
  m_selected = selected;
  m_hovered = hovered;
  m_disabled = disabled;
  setVisible(true);
  setEnabled(true);
  setSize(width, m_rowHeight);
  m_background->setSize(width, m_rowHeight);
  m_row->setSize(width, m_rowHeight);

  const bool thumbnailEligible = !entry.isDir && DirectoryScanner::isImagePath(entry.absPath);
  const std::string nextPath = thumbnailEligible ? entry.absPath.string() : std::string();
  if (m_thumbnailPath != nextPath) {
    releaseThumbnail();
    m_thumbnailPath = nextPath;
    if (!m_thumbnailPath.empty() && m_thumbnails != nullptr)
      (void)m_thumbnails->acquire(m_thumbnailPath, 64);
  }

  m_icon->setFile(entry.absPath, entry.isDir);
  m_image->setPosition(Style::spaceSm * m_scale, (m_rowHeight - m_iconSize) * 0.5F);
  m_image->setSize(m_iconSize, m_iconSize);
  refreshThumbnail(renderer);
  m_name->setText(entry.name);
  m_hidden = filemanager::isHiddenFileName(entry.name);
  // Name's column includes its icon and the icon-to-name gap. The Flex gap
  // between Name and Size is outside the column, just like the header gap.
  const float nameWidth = std::max(0.0F, m_nameColumnWidth * m_scale - m_iconSize - Style::spaceSm * m_scale);
  m_name->setMinWidth(nameWidth);
  m_name->setMaxWidth(nameWidth);
  m_size->setText(formatSize(entry));
  m_date->setText(formatFileTime(entry.mtime));

  applyVisualState();
  layout(renderer);
}

InputArea* FileEntryRow::beginInlineEdit(
    const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
    std::function<void()> cancel
) {
  m_editPath = path;
  m_name->setVisible(false);
  m_name->setParticipatesInLayout(false);
  m_editor->setVisible(true);
  m_editor->setParticipatesInLayout(true);
  m_editor->setValue(name);
  m_editor->setOnSubmit(std::move(submit));
  m_editor->setOnKeyEvent([cancel = std::move(cancel)](std::uint32_t sym, std::uint32_t) {
    if (sym != XKB_KEY_Escape)
      return false;
    cancel();
    return true;
  });
  const auto extension = name.rfind('.');
  if (extension != std::string::npos && extension > 0)
    m_editor->selectRange(0, extension);
  else
    m_editor->selectAll();
  return m_editor->inputArea();
}

void FileEntryRow::endInlineEdit(const std::string& path) {
  if (m_editPath != path)
    return;
  m_editPath.clear();
  m_editor->setVisible(false);
  m_editor->setParticipatesInLayout(false);
  m_name->setVisible(true);
  m_name->setParticipatesInLayout(true);
}

void FileEntryRow::clear() {
  releaseThumbnail();
  m_boundIndex = static_cast<std::size_t>(-1);
  m_selected = false;
  m_hovered = false;
  m_disabled = false;
  setVisible(false);
}

void FileEntryRow::setVisualState(bool selected, bool hovered, bool disabled) {
  if (m_selected == selected && m_hovered == hovered && m_disabled == disabled) {
    return;
  }
  m_selected = selected;
  m_hovered = hovered;
  m_disabled = disabled;
  applyVisualState();
}

void FileEntryRow::setSelectionRoles(ColorRole background, ColorRole foreground) {
  m_selectedBackgroundRole = background;
  m_selectedForegroundRole = foreground;
  applyVisualState();
}

void FileEntryRow::applyVisualState() {
  const Color bg = m_selected ? colorForRole(m_selectedBackgroundRole)
      : m_hovered             ? colorForRole(ColorRole::Hover)
                              : clearColor();
  const Color fg = m_selected ? colorForRole(m_selectedForegroundRole)
      : m_hovered             ? colorForRole(ColorRole::OnHover)
                              : colorForRole(ColorRole::OnSurface);
  const Color sub = m_selected ? colorForRole(m_selectedForegroundRole)
      : m_hovered              ? colorForRole(ColorRole::OnHover)
                               : colorForRole(ColorRole::OnSurfaceVariant);
  const float alpha = m_disabled ? 0.55F : 1.0F;
  const float iconNameAlpha = filemanager::fileEntryIconNameAlpha(m_hidden, m_selected, m_hovered, m_disabled);

  m_background->setFill(bg);
  m_icon->setColor(withAlpha(fg, iconNameAlpha));
  m_name->setColor(withAlpha(fg, iconNameAlpha));
  m_size->setColor(withAlpha(sub, alpha));
  m_date->setColor(withAlpha(sub, alpha));
}
