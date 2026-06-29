#pragma once

#include "render/scene/input_area.h"
#include "ui/palette.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class AsyncTextureCache;
class Box;
class Flex;
class Glyph;
class Image;
class Label;
class Renderer;

struct WindowSwitcherEntry {
  std::string windowId;
  std::string title;
  std::string appId;
  std::string appLabel;
  std::string iconPath;
  std::uintptr_t closeHandle = 0;
};

// Window-switcher cell: surface frame around a surface-variant icon card and title.
class WindowSwitcherTile : public InputArea {
public:
  WindowSwitcherTile(float contentScale, AsyncTextureCache* asyncTextures);

  [[nodiscard]] static bool
  hitTestCloseRegion(float cellWidth, float cellHeight, float contentScale, float localX, float localY) noexcept;

  void setCellSize(float cellWidth, float cellHeight);
  void setAppIconColorizeTint(std::optional<ColorSpec> tint) { m_appIconColorizeTint = tint; }
  void setOnInvalidate(std::function<void()> callback) { m_onInvalidate = std::move(callback); }
  void setCloseHovered(bool hovered);
  void bind(Renderer& renderer, const WindowSwitcherEntry& entry, bool selected, bool hovered);

private:
  void applyVisualState();
  void applyCloseVisualState();
  bool refreshIcon(Renderer& renderer);
  void layoutOverlays(Renderer& renderer);

protected:
  void doLayout(Renderer& renderer) override;

  float m_contentScale = 1.0f;
  float m_cellWidth = 0.0f;
  float m_cellHeight = 0.0f;
  float m_iconHostWidth = 0.0f;
  float m_iconHostHeight = 0.0f;

  Flex* m_layout = nullptr;
  Box* m_frame = nullptr;
  Flex* m_inner = nullptr;
  Box* m_iconHost = nullptr;
  Flex* m_caption = nullptr;
  Box* m_closeBackdrop = nullptr;
  Glyph* m_closeGlyph = nullptr;
  Image* m_icon = nullptr;
  Glyph* m_fallbackGlyph = nullptr;
  Label* m_title = nullptr;
  Label* m_subtitle = nullptr;

  WindowSwitcherEntry m_entry;
  bool m_hasEntry = false;
  bool m_selected = false;
  bool m_hovered = false;
  bool m_closeHovered = false;
  std::string m_iconPath;
  int m_iconTargetSize = 0;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::optional<ColorSpec> m_appIconColorizeTint;
  std::function<void()> m_onInvalidate;
};
