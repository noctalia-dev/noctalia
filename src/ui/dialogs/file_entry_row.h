#pragma once

#include "render/scene/input_area.h"

#include <cstddef>
#include <functional>

class Box;
class Flex;
struct FileEntry;
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
  /// Width of the checkbox hit zone from the row's left edge.
  ///
  /// Multi-selection stays reachable without the keyboard: the checkbox builds a
  /// set on its own, so Ctrl/Shift are accelerators rather than the only way in.
  /// The grid hit-tests the zone through VirtualGridAdapter::onPointerPress,
  /// which needs this measurement.
  [[nodiscard]] static float checkboxZoneWidth(float scale);
  void bind(
      Renderer& renderer, const FileEntry& entry, std::size_t index, float width, bool selected, bool hovered,
      bool disabled
  );
  /// Show a checkbox ahead of the icon. Multi-selection is otherwise invisible
  /// until something is already selected, which leaves the user guessing whether
  /// picking several is even possible.
  void setMultiSelect(bool enabled);
  void clear();
  void setVisualState(bool selected, bool hovered, bool disabled);

private:
  void applyVisualState();

  float m_scale = 1.0F;
  float m_rowHeight = 0.0F;
  std::size_t m_boundIndex = static_cast<std::size_t>(-1);
  bool m_selected = false;
  bool m_hovered = false;
  bool m_disabled = false;
  bool m_multiSelect = false;
  Box* m_background = nullptr;
  Flex* m_row = nullptr;
  Glyph* m_check = nullptr;
  Glyph* m_icon = nullptr;
  Label* m_name = nullptr;
  Label* m_size = nullptr;
  Label* m_date = nullptr;
  IndexCallback m_onClick;
  IndexCallback m_onMotion;
  IndexCallback m_onEnter;
  IndexCallback m_onLeave;
};
