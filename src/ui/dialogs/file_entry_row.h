#pragma once

#include "render/scene/input_area.h"
#include "ui/palette.h"

#include <cstddef>
#include <functional>
#include <string>

class Box;
class Flex;
struct FileEntry;
class Glyph;
class FileIcon;
class Image;
class Label;
class Renderer;
class ThumbnailService;
class Input;

class FileEntryRow final : public InputArea {
public:
  using IndexCallback = std::function<void(std::size_t)>;

  explicit FileEntryRow(float scale, float iconSize = 0.0F, ThumbnailService* thumbnails = nullptr);
  ~FileEntryRow() override;

  [[nodiscard]] std::size_t boundIndex() const noexcept { return m_boundIndex; }
  [[nodiscard]] float rowHeight() const noexcept { return m_rowHeight; }

  void setCallbacks(IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave);
  void setColumnWidths(float name, float size, float modified);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
      bool disabled
  );
  void clear();
  void setVisualState(bool selected, bool hovered, bool disabled);
  void setSelectionRoles(ColorRole background, ColorRole foreground);
  [[nodiscard]] InputArea* beginInlineEdit(
      const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel
  );
  void endInlineEdit(const std::string& path);

private:
  void applyVisualState();
  void refreshThumbnail(Renderer& renderer);
  void releaseThumbnail();

  float m_scale = 1.0F;
  float m_rowHeight = 0.0F;
  float m_iconSize = 0.0F;
  float m_sizeColumnWidth = 96.0F;
  float m_dateColumnWidth = 152.0F;
  float m_nameColumnWidth = 0.0F;
  std::size_t m_boundIndex = static_cast<std::size_t>(-1);
  bool m_selected = false;
  bool m_hovered = false;
  bool m_disabled = false;
  bool m_hidden = false;
  ColorRole m_selectedBackgroundRole = ColorRole::Primary;
  ColorRole m_selectedForegroundRole = ColorRole::OnPrimary;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  FileIcon* m_icon = nullptr;
  Image* m_image = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  std::string m_thumbnailPath;
  Label* m_name = nullptr;
  Label* m_size = nullptr;
  Label* m_date = nullptr;
  Input* m_editor = nullptr;
  std::string m_editPath;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
