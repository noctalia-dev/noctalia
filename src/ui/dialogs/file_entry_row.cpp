#include "ui/dialogs/file_entry_row.h"

#include "core/files/directory_scanner.h"
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
#include <gio/gio.h>
#include <memory>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  using filemanager::columnIndex;
  using filemanager::FileColumn;

  std::string formatFileSize(const FileEntry& entry) {
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

  for (const auto column : filemanager::kFileColumns) {
    const auto index = columnIndex(column);
    auto cell = std::make_unique<Flex>();
    cell->setAlign(FlexAlign::Center);
    cell->setClipChildren(true);
    const bool visible = column <= FileColumn::Modified;
    cell->setVisible(visible);
    cell->setParticipatesInLayout(visible);
    m_cells[index] = static_cast<Flex*>(m_row->addChild(std::move(cell)));
    if (column == FileColumn::Name)
      continue;
    const float width = filemanager::FileColumns{}[column] * scale;
    m_cells[index]->setMinWidth(width);
    m_cells[index]->setMaxWidth(width);
    auto label = ui::label({
        .fontSize = Style::fontSizeCaption * scale,
        .maxLines = 1,
        .textAlign = column == FileColumn::Type ? TextAlign::Start : TextAlign::End,
        .flexGrow = 1.0F,
    });
    m_columnLabels[index] = static_cast<Label*>(m_cells[index]->addChild(std::move(label)));
  }

  auto* nameCell = m_cells[columnIndex(FileColumn::Name)];
  nameCell->setGap(Style::spaceSm * scale);
  auto icon = std::make_unique<FileIcon>();
  icon->setGlyphSize(m_iconSize);
  m_icon = static_cast<FileIcon*>(nameCell->addChild(std::move(icon)));
  auto image = ui::image({.fit = ImageFit::Contain, .visible = false});
  m_image = static_cast<Image*>(nameCell->addChild(std::move(image)));
  // The thumbnail and file icon share the same artwork slot.

  auto name = ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .flexGrow = 1.0F,
  });
  m_name = static_cast<Label*>(nameCell->addChild(std::move(name)));

  auto editor = std::make_unique<Input>();
  m_editor = editor.get();
  m_editor->setControlHeight(28.0F * scale);
  m_editor->setHorizontalPadding(4.0F * scale);
  m_editor->setVisible(false);
  m_editor->setParticipatesInLayout(false);
  m_editor->setFlexGrow(1.0F);
  nameCell->addChild(std::move(editor));

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

void FileEntryRow::setColumns(std::span<const FileColumn> columns, const filemanager::FileColumns& widths) {
  const bool orderChanged = !std::ranges::equal(columns, m_columns);
  if (orderChanged) {
    m_columns.assign(columns.begin(), columns.end());
    for (auto column : filemanager::kFileColumns) {
      auto* cell = m_cells[columnIndex(column)];
      const bool visible = std::ranges::contains(columns, column);
      cell->setVisible(visible);
      cell->setParticipatesInLayout(visible);
    }
    for (auto column : columns)
      m_row->addChild(m_row->removeChild(m_cells[columnIndex(column)]));
  }
  for (auto column : columns) {
    auto* cell = m_cells[columnIndex(column)];
    const float width = widths[column] * m_scale;
    cell->setMinWidth(width);
    cell->setMaxWidth(width);
    if ((orderChanged || !m_columnsConfigured) && column != FileColumn::Name) {
      const float inset = filemanager::fileColumnTextInset(column == columns.back(), Style::spaceSm) * m_scale;
      cell->setPadding(0.0F, Style::rtl() ? 0.0F : inset, 0.0F, Style::rtl() ? inset : 0.0F);
    }
  }
  m_columnsConfigured = true;
}

void FileEntryRow::bind(
    Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
    bool disabled, std::optional<std::uint64_t> folderItems
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

  const bool thumbnailEligible = !entry.isDir && DirectoryScanner::isThumbnailPath(entry.absPath);
  const std::string nextPath = thumbnailEligible ? entry.absPath.string() : std::string();
  if (m_thumbnailPath != nextPath) {
    releaseThumbnail();
    m_thumbnailPath = nextPath;
    if (!m_thumbnailPath.empty() && m_thumbnails != nullptr)
      (void)m_thumbnails->acquire(m_thumbnailPath, 64);
  }

  m_icon->setFile(entry.absPath, entry.isDir);
  m_image->setSize(m_iconSize, m_iconSize);
  refreshThumbnail(renderer);
  m_name->setText(entry.name);
  m_hidden = filemanager::isHiddenFileName(entry.name);
  for (auto column : m_columns) {
    auto* label = m_columnLabels[columnIndex(column)];
    switch (column) {
    case FileColumn::Name:
      break;
    case FileColumn::Size:
      if (!entry.isDir)
        label->setText(formatFileSize(entry));
      else if (!m_columnsConfigured)
        label->setText(i18n::tr("ui.dialogs.file.entry.folder"));
      else if (folderItems)
        label->setText(i18n::trp("ui.dialogs.file.entry.folder-items", static_cast<long>(*folderItems)));
      else
        label->setText("—");
      break;
    case FileColumn::Modified:
      label->setText(formatFileTime(entry.mtime));
      break;
    case FileColumn::Type: {
      g_autofree char* description = entry.type.empty() ? nullptr : g_content_type_get_description(entry.type.c_str());
      label->setText(description ? description : (entry.type.empty() ? "—" : entry.type));
      break;
    }
    case FileColumn::Created:
    case FileColumn::Accessed: {
      const bool created = column == FileColumn::Created;
      label->setText(
          (created ? entry.hasCreated : entry.hasAccessed)
              ? formatLocalUnixTime(
                    static_cast<std::int64_t>(created ? entry.created : entry.accessed), "%Y-%m-%d %H:%M"
                )
              : "—"
      );
      break;
    }
    }
  }

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
  m_editor->setOnSubmit(submit);
  // The standalone file manager does not install the shell's Validate matcher.
  m_editor->setOnKeyEvent([this, submit = std::move(submit),
                           cancel = std::move(cancel)](std::uint32_t sym, std::uint32_t) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      const auto value = m_editor->value();
      submit(value);
      return true;
    }
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
  for (auto* label : m_columnLabels)
    if (label)
      label->setColor(withAlpha(sub, alpha));
}
