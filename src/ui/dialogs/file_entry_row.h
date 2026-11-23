#pragma once

#include "filemanager/file_columns.h"
#include "render/scene/input_area.h"
#include "ui/palette.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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
  void setColumns(std::span<const filemanager::FileColumn> columns, const filemanager::FileColumns& widths);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
      bool disabled, std::optional<std::uint64_t> folderItems = std::nullopt, bool cut = false,
      std::optional<std::string> protectedThumbnail = {}, bool focused = false
  );
  void clear();
  void setVisualState(bool selected, bool hovered, bool disabled, bool cut = false, bool focused = false);
  [[nodiscard]] bool isCut() const noexcept { return m_cut; }
  void setSelectionRoles(ColorRole background, ColorRole foreground);
  void setAllowThumbnailsAndMetadata(bool allow) noexcept { m_allowThumbnailsAndMetadata = allow; }
  [[nodiscard]] bool allowThumbnailsAndMetadata() const noexcept { return m_allowThumbnailsAndMetadata; }
  [[nodiscard]] FileIcon* fileIcon() const noexcept { return m_icon; }
  [[nodiscard]] InputArea* beginInlineEdit(
      const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel, bool isDir = false
  );
  void endInlineEdit(const std::string& path);

private:
  void applyVisualState();
  void refreshThumbnail(Renderer& renderer);
  void releaseThumbnail();

  float m_scale = 1.0F;
  float m_rowHeight = 0.0F;
  float m_iconSize = 0.0F;
  std::array<Flex*, filemanager::kFileColumns.size()> m_cells{};
  std::array<Label*, filemanager::kFileColumns.size()> m_columnLabels{};
  std::vector<filemanager::FileColumn> m_columns{
      filemanager::kDefaultFileColumns.begin(), filemanager::kDefaultFileColumns.end()
  };
  bool m_columnsConfigured = false;
  std::size_t m_boundIndex = static_cast<std::size_t>(-1);
  bool m_selected = false;
  bool m_hovered = false;
  bool m_disabled = false;
  bool m_cut = false;
  bool m_focused = false;
  bool m_hidden = false;
  bool m_allowThumbnailsAndMetadata = true;
  ColorRole m_selectedBackgroundRole = ColorRole::Primary;
  ColorRole m_selectedForegroundRole = ColorRole::OnPrimary;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  FileIcon* m_icon = nullptr;
  Image* m_image = nullptr;
  ThumbnailService* m_thumbnails = nullptr;
  std::string m_thumbnailPath;
  bool m_thumbnailDiskCache = true;
  Label* m_name = nullptr;
  Input* m_editor = nullptr;
  std::string m_editPath;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
