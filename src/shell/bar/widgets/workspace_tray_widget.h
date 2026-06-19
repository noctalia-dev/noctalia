#pragma once

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "render/core/renderer.h"
#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Glyph;
class InputArea;
class Label;
struct wl_output;

class WorkspaceTrayWidget : public Widget {
public:
  enum class DisplayMode : std::uint8_t {
    Id,
    Name,
  };

  WorkspaceTrayWidget(
      CompositorPlatform& platform, wl_output* output, DisplayMode displayMode, std::size_t maxLabelChars,
      ColorSpec focusedColor, ColorSpec occupiedColor, bool showChevron, bool focusedOnly, bool hideWhenEmpty
  );
  ~WorkspaceTrayWidget() override = default;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild(Renderer& renderer);
  void syncWidgetVisibility(bool showWidget);

  [[nodiscard]] std::optional<std::size_t> activeWorkspaceIndex() const;
  void activateAdjacentWorkspace(int direction);

  [[nodiscard]] std::string activeWorkspaceLabel() const;
  [[nodiscard]] bool isFocusedOutput() const;
  [[nodiscard]] ColorSpec textColor() const;
  [[nodiscard]] std::string outputContext() const;

  CompositorPlatform& m_platform;
  wl_output* m_output = nullptr;
  DisplayMode m_displayMode = DisplayMode::Id;
  std::size_t m_maxLabelChars = 3;
  ColorSpec m_focusedColor = colorSpecFromRole(ColorRole::Primary);
  ColorSpec m_occupiedColor = colorSpecFromRole(ColorRole::Secondary);
  bool m_showChevron = true;
  bool m_focusedOnly = false;
  bool m_hideWhenEmpty = false;
  bool m_wasFocusedOutput = true;

  std::vector<Workspace> m_cachedState;
  bool m_rebuildPending = true;
  std::uint64_t m_textMetricsGeneration = 0;

  InputArea* m_container = nullptr;
  Label* m_label = nullptr;
  Glyph* m_chevron = nullptr;
};
