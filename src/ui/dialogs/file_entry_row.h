#pragma once

#include "core/files/directory_scanner.h"
#include "render/scene/input_area.h"

#include <cstddef>
#include <functional>

class Box;
class Flex;
class Glyph;
class Label;
class Renderer;

class FileEntryRow final : public InputArea {
public:
  using IndexCallback = std::function<void(std::size_t)>;

  explicit FileEntryRow(float scale);

  [[nodiscard]] std::size_t boundIndex() const noexcept { return m_boundIndex; }
  [[nodiscard]] float rowHeight() const noexcept { return m_rowHeight; }

  void setCallbacks(IndexCallback onClick, IndexCallback onMotion, IndexCallback onEnter, IndexCallback onLeave);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
      bool disabled
  );
  void clear();
  void setVisualState(bool selected, bool hovered, bool disabled);

private:
  void applyVisualState();

  float m_scale = 1.0f;
  float m_rowHeight = 0.0f;
  std::size_t m_boundIndex = static_cast<std::size_t>(-1);
  bool m_selected = false;
  bool m_hovered = false;
  bool m_disabled = false;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  Glyph* m_icon = nullptr;
  Label* m_name = nullptr;
  Label* m_size = nullptr;
  Label* m_date = nullptr;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
