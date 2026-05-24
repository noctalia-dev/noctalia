#include "ui/dialogs/file_entry_row.h"

#include "core/files/directory_scanner.h"
#include "i18n/i18n.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>

namespace {

  constexpr float kSizeColumnWidth = 96.0f;
  constexpr float kDateColumnWidth = 152.0f;

  std::string formatSize(const FileEntry& entry) {
    if (entry.isDir) {
      return i18n::tr("ui.dialogs.file.entry.folder");
    }

    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(entry.size);
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

FileEntryRow::FileEntryRow(float scale) : m_scale(scale), m_rowHeight(std::ceil(32.0f * scale)) {
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
      .radius = Style::scaledRadiusMd(scale),
  });
  m_background = static_cast<Box*>(addChild(std::move(background)));

  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .configure = [scale](Flex& container) { container.setPadding(Style::spaceXs * scale, Style::spaceSm * scale); },
  });
  m_row = static_cast<Flex*>(addChild(std::move(row)));

  auto icon = ui::glyph({
      .glyphSize = Style::fontSizeBody * scale,
  });
  m_icon = static_cast<Glyph*>(m_row->addChild(std::move(icon)));

  auto name = ui::label({
      .fontSize = Style::fontSizeBody * scale,
      .maxLines = 1,
      .flexGrow = 1.0f,
  });
  m_name = static_cast<Label*>(m_row->addChild(std::move(name)));

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

void FileEntryRow::setCallbacks(
    IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave
) {
  m_onClick = std::move(onClick);
  m_onMotion = std::move(onMotion);
  m_onEnter = std::move(onEnter);
  m_onLeave = std::move(onLeave);
}

void FileEntryRow::bind(
    Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
    bool disabled
) {
  m_boundIndex = index;
  m_selected = selected;
  m_hovered = hovered;
  m_disabled = disabled;
  setVisible(true);
  setEnabled(true);
  setSize(width, m_rowHeight);
  m_background->setSize(width, m_rowHeight);
  m_row->setSize(width, m_rowHeight);

  m_icon->setGlyph(entry.isDir ? "folder" : (DirectoryScanner::isImagePath(entry.absPath) ? "image" : "file"));
  m_name->setText(entry.name);
  m_name->setMaxWidth(std::max(0.0f, width - (kSizeColumnWidth + kDateColumnWidth + 72.0f) * m_scale));
  m_size->setText(formatSize(entry));
  m_date->setText(formatFileTime(entry.mtime));

  applyVisualState();
  layout(renderer);
}

void FileEntryRow::clear() {
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

void FileEntryRow::applyVisualState() {
  const Color bg = m_selected  ? colorForRole(ColorRole::Primary)
                   : m_hovered ? colorForRole(ColorRole::Hover)
                               : clearColor();
  const Color fg = m_selected  ? colorForRole(ColorRole::OnPrimary)
                   : m_hovered ? colorForRole(ColorRole::OnHover)
                               : colorForRole(ColorRole::OnSurface);
  const Color sub = m_selected  ? colorForRole(ColorRole::OnPrimary)
                    : m_hovered ? colorForRole(ColorRole::OnHover)
                                : colorForRole(ColorRole::OnSurfaceVariant);
  const float alpha = m_disabled ? 0.55f : 1.0f;

  m_background->setFill(bg);
  m_icon->setColor(withAlpha(fg, alpha));
  m_name->setColor(withAlpha(fg, alpha));
  m_size->setColor(withAlpha(sub, alpha));
  m_date->setColor(withAlpha(sub, alpha));
}
