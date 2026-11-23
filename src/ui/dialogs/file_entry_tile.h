#pragma once

#include "render/scene/input_area.h"
#include "ui/palette.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

class Box;
struct FileEntry;
class Glyph;
class FileIcon;
class Image;
class Label;
class Input;
class Renderer;
class ThumbnailService;

class FileEntryTile final : public InputArea {
public:
  using IndexCallback = std::function<void(std::size_t)>;

  FileEntryTile(float scale, ThumbnailService* thumbnails, float iconSize = 0.0F);
  ~FileEntryTile() override;

  [[nodiscard]] std::size_t boundIndex() const noexcept { return m_boundIndex; }
  [[nodiscard]] const std::string& thumbnailPath() const noexcept { return m_thumbnailPath; }

  void setCallbacks(IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, float height, bool selected,
      bool hovered, bool disabled, bool cut = false, std::optional<std::string> protectedThumbnail = {},
      bool focused = false
  );
  void refreshThumbnail(Renderer& renderer);
  void clear(Renderer& renderer);
  void setVisualState(bool selected, bool hovered, bool disabled, bool cut = false, bool focused = false);
  [[nodiscard]] bool isCut() const noexcept { return m_cut; }
  void setSelectionRoles(ColorRole background, ColorRole foreground);
  void setAllowThumbnailsAndMetadata(bool allow) noexcept { m_allowThumbnailsAndMetadata = allow; }
  [[nodiscard]] bool allowThumbnailsAndMetadata() const noexcept { return m_allowThumbnailsAndMetadata; }
  [[nodiscard]] FileIcon* fileIcon() const noexcept { return m_glyph; }
  [[nodiscard]] InputArea* beginInlineEdit(
      const std::string& path, const std::string& name, std::function<void(const std::string&)> submit,
      std::function<void()> cancel, bool isDir = false
  );
  void endInlineEdit(const std::string& path);

private:
  void doLayout(Renderer& renderer) override;
  void applyVisualState();
  void releaseThumbnail();

  float m_scale = 1.0F;
  float m_iconSize = 0.0F;
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
  bool m_thumbnailEligible = false;
  ThumbnailService* m_thumbnails = nullptr;
  Box* m_background = nullptr;
  Box* m_preview = nullptr;
  Image* m_image = nullptr;
  FileIcon* m_glyph = nullptr;
  Label* m_label = nullptr;
  Input* m_editor = nullptr;
  std::string m_editPath;
  std::string m_thumbnailPath;
  bool m_thumbnailDiskCache = true;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
