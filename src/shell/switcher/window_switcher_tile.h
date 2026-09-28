#pragma once

#include "capture/screencopy_capture.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "shell/switcher/window_switcher_style.h"
#include "ui/palette.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class AsyncTextureCache;
class Box;
class Button;
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
  std::uintptr_t captureHandle = 0;
  std::shared_ptr<const ScreencopyImage> thumbnail;
};

// Preview card shared by window-switcher presentation styles.
class WindowSwitcherTile : public InputArea {
public:
  WindowSwitcherTile(float contentScale, AsyncTextureCache* asyncTextures);

  void setCardSize(float width, float height);
  void setShadowStyle(const RoundedRectStyle& style);
  void setShowCaption(bool show);
  void setShowAppIcon(bool show);
  void setAppIconColorizeTint(std::optional<ColorSpec> tint) { m_appIconColorizeTint = tint; }
  void setOnInvalidate(std::function<void()> callback) { m_onInvalidate = std::move(callback); }
  void setOnActivate(std::function<void()> callback) { m_onActivate = std::move(callback); }
  void setOnClose(std::function<void()> callback) { m_onClose = std::move(callback); }
  void bind(
      Renderer& renderer, const WindowSwitcherEntry& entry, WindowSwitcherTileDepth depth, bool showCaption,
      bool wideCaption, WindowSwitcherHorizontalPlacement iconPlacement,
      WindowSwitcherHorizontalPlacement closePlacement
  );

private:
  bool refreshIcon(Renderer& renderer);
  void setPointerHovered(bool hovered);
  void applyVisualState();
  void layoutContent(Renderer& renderer);

protected:
  void doLayout(Renderer& renderer) override;

  float m_contentScale = 1.0F;
  float m_cardWidth = 0.0F;
  float m_cardHeight = 0.0F;

  Box* m_shadow = nullptr;
  Box* m_frame = nullptr;
  Box* m_previewHost = nullptr;
  Image* m_thumbnail = nullptr;
  Box* m_toneOverlay = nullptr;
  Image* m_icon = nullptr;
  Glyph* m_fallbackGlyph = nullptr;
  Box* m_captionBadge = nullptr;
  Flex* m_caption = nullptr;
  Label* m_title = nullptr;
  Label* m_subtitle = nullptr;
  Button* m_close = nullptr;

  bool m_hasEntry = false;
  bool m_selected = false;
  bool m_pointerHovered = false;
  bool m_shadowConfigured = false;
  bool m_showCaption = true;
  bool m_captionVisible = false;
  bool m_wideCaption = false;
  bool m_showAppIcon = true;
  WindowSwitcherHorizontalPlacement m_iconPlacement = WindowSwitcherHorizontalPlacement::Left;
  WindowSwitcherHorizontalPlacement m_closePlacement = WindowSwitcherHorizontalPlacement::Right;
  WindowSwitcherTileDepth m_depth = WindowSwitcherTileDepth::Far;
  RoundedRectStyle m_shadowStyle;
  std::string m_iconPath;
  int m_iconTargetSize = 0;
  std::shared_ptr<const ScreencopyImage> m_thumbnailImage;
  AsyncTextureCache* m_asyncTextures = nullptr;
  std::optional<ColorSpec> m_appIconColorizeTint;
  std::function<void()> m_onInvalidate;
  std::function<void()> m_onActivate;
  std::function<void()> m_onClose;
};
