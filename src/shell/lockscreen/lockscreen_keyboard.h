#pragma once

#include "shell/lockscreen/lockscreen_keyboard_layout.h"

#include <functional>
#include <memory>
#include <vector>

class Button;
class Flex;
class Renderer;
struct LayoutRect;

// On-screen keyboard drawn on the lock surface. The session lock hides every other client's
// surfaces, so an external OSK cannot appear here; this one is plain noctalia controls and feeds
// synthesized key events back to its owner.
class LockscreenKeyboard {
public:
  using KeyCallback = std::function<void(const lockscreen_keyboard::KeyOutput&)>;

  LockscreenKeyboard() = default;
  LockscreenKeyboard(const LockscreenKeyboard&) = delete;
  LockscreenKeyboard& operator=(const LockscreenKeyboard&) = delete;

  // Builds the keyboard panel. The caller adds the returned node to its scene and keeps it alive at
  // least as long as this object is used.
  [[nodiscard]] std::unique_ptr<Flex> build(KeyCallback onKey, std::function<void()> onHide);

  // Preferred height for a given panel width.
  [[nodiscard]] static float preferredHeight(float width) noexcept;

  void setVisible(bool visible);
  [[nodiscard]] bool visible() const noexcept { return m_visible; }
  void setRadius(float panelRadius, float keyRadius);
  void reset(); // back to the lowercase layer
  void arrange(Renderer& renderer, const LayoutRect& rect);

private:
  struct KeySlot {
    Button* button = nullptr;
    std::size_t row = 0;
    std::size_t index = 0;
  };

  void handlePress(std::size_t row, std::size_t index);
  void applyLayer();

  Flex* m_panel = nullptr;
  std::vector<Flex*> m_rows;
  std::vector<KeySlot> m_keys;
  lockscreen_keyboard::Layer m_layer = lockscreen_keyboard::Layer::Lower;
  KeyCallback m_onKey;
  std::function<void()> m_onHide;
  float m_keyRadius = 6.0F;
  bool m_visible = false;
};
