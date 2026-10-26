#pragma once

#include "ipc/pixel_stream_client.h"
#include "render/core/texture_handle.h"
#include "render/scene/input_area.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

class Image;
class PixelStreamService;

// Displays frames from a companion process (see PixelStreamClient) aspect-fit inside its box,
// and reports pointer and keyboard input in image coordinates. Socket I/O runs in
// PixelStreamService; uploads happen only in layout, and each upload requests the next frame,
// so the stream is paced by how often this control is actually laid out.
class PixelStreamView final : public InputArea {
public:
  enum class PointerKind : std::uint8_t { Move, Down, Up, Cancel, RightClick, MiddleClick, Scroll };

  struct PointerEvent {
    PointerKind kind = PointerKind::Move;
    // Position on the displayed image, 0..1 from the top-left; zero for Cancel.
    float x = 0.0F;
    float y = 0.0F;
    // Whole wheel steps for Scroll, positive right/down.
    float dx = 0.0F;
    float dy = 0.0F;
    // Changes after every event other than Move. Moves sharing a segment may be coalesced
    // without reordering them across a press, release, click, scroll, or cancel.
    std::uint64_t segment = 0;
  };
  using PointerHandler = std::function<void(const PointerEvent& event)>;
  using KeyHandler = std::function<void(const KeyData& key)>;

  PixelStreamView();
  ~PixelStreamView() override;
  PixelStreamView(const PixelStreamView&) = delete;
  PixelStreamView& operator=(const PixelStreamView&) = delete;

  void setPath(const std::string& path);
  void setRadius(float radius);
  // `buttons` is an InputArea button mask: left enables drags (Down/Move/Up/Cancel), right and
  // middle enable their clicks. Hover Moves are reported whenever a handler is set.
  void setPointerHandler(PointerHandler handler, std::uint32_t buttons, bool scroll);
  // A key handler makes the view focusable by clicking it.
  void setKeyHandler(KeyHandler handler);
  // Ends an in-progress left drag with a Cancel event.
  void cancelDrag();

private:
  void onStreamChanged();
  [[nodiscard]] std::optional<std::pair<float, float>> imagePosition(float localX, float localY) const;
  void emit(PointerKind kind, float x = 0.0F, float y = 0.0F, float dx = 0.0F, float dy = 0.0F);
  void updateAcceptedButtons();
  void doLayout(Renderer& renderer) override;
  void doInvalidateGpuResources(Renderer& renderer) override;
  void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }

  PixelStreamClient m_client;
  PixelStreamService* m_service = nullptr;
  Renderer* m_renderer = nullptr;
  Image* m_image = nullptr;
  TextureHandle m_texture;
  bool m_clearPending = false;
  // Size of the frame on screen, which pointer positions map onto.
  int m_displayWidth = 0;
  int m_displayHeight = 0;

  PointerHandler m_pointerHandler;
  KeyHandler m_keyHandler;
  std::uint32_t m_buttons = 0;
  bool m_scroll = false;
  bool m_dragging = false;
  std::uint32_t m_clickButton = 0;
  std::pair<float, float> m_clickPosition;
  std::uint64_t m_segment = 0;
};
