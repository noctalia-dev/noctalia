#pragma once

#include "shell/bar/widget.h"

#include <cstdint>

class Glyph;
class Label;
class Renderer;
class LockKeysService;

class LockKeysWidget : public Widget {
public:
  enum class DisplayMode : std::uint8_t { Short = 0, Full = 1 };

  LockKeysWidget(
      LockKeysService* lockKeys, bool showCapsLock, bool showNumLock, bool showScrollLock, bool hideWhenOff,
      DisplayMode displayMode
  );

  static DisplayMode parseDisplayMode(const std::string& value);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void sync(Renderer& renderer);

  LockKeysService* m_lockKeys = nullptr;
  bool m_showCapsLock = true;
  bool m_showNumLock = true;
  bool m_showScrollLock = false;
  bool m_hideWhenOff = false;
  DisplayMode m_displayMode = DisplayMode::Short;

  Label* m_capsLabel = nullptr;
  Label* m_numLabel = nullptr;
  Label* m_scrollLabel = nullptr;
  Glyph* m_glyph = nullptr;

  struct CachedState {
    bool capsLock = false;
    bool numLock = false;
    bool scrollLock = false;
    bool anyVisible = false;

    bool operator==(const CachedState&) const = default;
  };

  CachedState m_cachedState;
  bool m_hasState = false;
  bool m_isVertical = false;
};
