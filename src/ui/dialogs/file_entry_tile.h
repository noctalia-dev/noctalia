#pragma once

#include "render/scene/input_area.h"

#include <cstddef>
#include <functional>
#include <string>

class Box;
struct FileEntry;
class Glyph;
class Image;
class Label;
class Renderer;
class ThumbnailService;

class FileEntryTile final : public InputArea {
public:
  using IndexCallback = std::function<void(std::size_t)>;

  FileEntryTile(float scale, ThumbnailService* thumbnails);
  ~FileEntryTile() override;

  [[nodiscard]] std::size_t boundIndex() const noexcept { return m_boundIndex; }
  [[nodiscard]] const std::string& thumbnailPath() const noexcept { return m_thumbnailPath; }

  void setCallbacks(IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, float height, bool selected,
      bool hovered, bool disabled
  );
  void refreshThumbnail(Renderer& renderer);
  void clear(Renderer& renderer);
  void setVisualState(bool selected, bool hovered, bool disabled);

private:
  void doLayout(Renderer& renderer) override;
  void applyVisualState();
  void releaseThumbnail();

  float m_scale = 1.0f;
  std::size_t m_boundIndex = static_cast<std::size_t>(-1);
  bool m_selected = false;
  bool m_hovered = false;
  bool m_disabled = false;
  bool m_thumbnailEligible = false;
  ThumbnailService* m_thumbnails = nullptr;
  Box* m_background = nullptr;
  Box* m_preview = nullptr;
  Image* m_image = nullptr;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_thumbnailPath;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
