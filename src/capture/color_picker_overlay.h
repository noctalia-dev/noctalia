#pragma once

#include "capture/screenshot_image.h"
#include "render/core/color.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class RenderContext;
class Renderer;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

namespace capture {

  // The sampled pixel's exact bytes from the frozen frame.
  struct PickedColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    [[nodiscard]] constexpr Color color() const noexcept {
      return rgbHex((static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b);
    }
  };

  // Frozen-screen color picker: the pointer is replaced by a loupe that magnifies the device pixels around
  // the sampled one. Pointer motion aims, arrow keys nudge by device pixels.
  class ColorPickerOverlay {
  public:
    // nullopt = cancelled (cancel keybind or output change).
    using PickCallback = std::function<void(std::optional<PickedColor>)>;
    using FailureCallback = std::function<void(const std::string& message)>;

    ColorPickerOverlay();
    ~ColorPickerOverlay();

    void initialize(WaylandConnection& wayland, RenderContext* renderContext);
    void setPickCallback(PickCallback callback);
    void setFailureCallback(FailureCallback callback);
    // Display labels of the configured keybinds, for the hint bar.
    struct KeybindLabels {
      std::string pick;   // [keybinds].validate
      std::string move;   // left/up/down/right, joined
      std::string cancel; // [keybinds].cancel
    };
    void setKeybindLabels(KeybindLabels labels);
    // Takes the frozen frames. The loupe stays hidden until the pointer enters; if an arrow key comes first,
    // it starts at the center pixel of initialOutput (or of the first frame's output when that has no frame).
    void begin(std::vector<FrozenScreenshot> screenshots, wl_output* initialOutput);
    // Immediate teardown without a callback, for when another capture mode takes over.
    void cancel();
    void onOutputChange();

    [[nodiscard]] bool isActive() const noexcept { return m_active; }
    bool onPointerEvent(const PointerEvent& event);
    // Presses only.
    void onKeyboardEvent(const KeyboardEvent& event);

  private:
    struct Instance;

    void ensureSurfaces();
    void destroySurfaces();
    bool surfacesMatchOutputs() const;
    void prepareFrame(Instance& instance);
    void abortWithError(const std::string& message);
    void setSampleFromPointer(wl_output* output, float localX, float localY);
    void nudgeSample(int dx, int dy);
    // Clamps to the output's frame, records the arrow-key offset from the pointer, and refreshes the loupe.
    void moveSample(wl_output* output, int x, int y);
    void refreshLoupes();
    void refreshLoupe(Instance& instance);
    void uploadZoom(Instance& instance, Renderer& renderer);
    // Sizes the loupe for the current zoom level on whole device pixels.
    void layoutLoupe(Instance& instance);
    // Steps the zoom level; negative zooms in (fewer, larger pixels).
    void changeZoom(int direction);
    // Puts the usage hint at the bottom or top edge, whichever the loupe and its readout are clear of.
    void placeHint(Instance& instance);
    void finishLater(std::optional<PickedColor> result);
    [[nodiscard]] const ScreencopyImage* frozenImage(wl_output* output) const;
    [[nodiscard]] std::optional<PickedColor> currentColor() const;

    WaylandConnection* m_wayland = nullptr;
    RenderContext* m_renderContext = nullptr;
    PickCallback m_onPick;
    FailureCallback m_onFailure;
    KeybindLabels m_keybindLabels;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::vector<FrozenScreenshot> m_frozenScreenshots;
    // Sampled device pixel in m_sampleOutput's frozen frame.
    wl_output* m_sampleOutput = nullptr;
    int m_sampleX = 0;
    int m_sampleY = 0;
    // Device pixel under the hidden pointer, and the arrow-key offset of the sample from it. Pointer motion keeps
    // the offset, so the loupe moves on from a nudged pixel instead of snapping back.
    wl_output* m_pointerOutput = nullptr;
    int m_pointerX = 0;
    int m_pointerY = 0;
    int m_nudgeX = 0;
    int m_nudgeY = 0;
    // Hidden until the first pointer event or arrow key places it.
    bool m_loupeShown = false;
    // Index into the overlay's zoom levels; reset on every begin().
    std::size_t m_zoomIndex = 0;
    bool m_active = false;
  };

} // namespace capture
