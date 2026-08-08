#pragma once

#include "capture/screencopy_capture.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace capture {

  enum class ConfirmAction { None, ForceClipboard, ForceSave };
  enum class DragMode {
    None,
    NewSelection,
    TopEdge,
    BottomEdge,
    LeftEdge,
    RightEdge,
    TopLeftCorner,
    TopRightCorner,
    BottomLeftCorner,
    BottomRightCorner,
    Move
  };

  struct FrozenScreenshot {
    wl_output* output = nullptr;
    ScreencopyImage image;
  };

  class ScreenshotRegionOverlay {
  public:
    using CompleteCallback = std::function<void(std::optional<LogicalRect>, wl_output* output, ConfirmAction action)>;
    using FailureCallback = std::function<void(const std::string& message)>;

    ScreenshotRegionOverlay();
    ~ScreenshotRegionOverlay();

    void initialize(WaylandConnection& wayland, RenderContext* renderContext);
    void setCompleteCallback(CompleteCallback callback);
    void setFailureCallback(FailureCallback callback);
    void setConfirmKeybindLabels(std::string copyLabel, std::string saveLabel, std::string cancelLabel);
    void setFrozenScreenshots(std::vector<FrozenScreenshot> screenshots);
    [[nodiscard]] std::vector<FrozenScreenshot> takeFrozenScreenshots();
    void begin(
        bool freezeScreen, bool fullscreenPick = false, bool confirmRegion = false,
        std::optional<LogicalRect> initialRegion = std::nullopt
    );
    void cancel();
    void cancelSelection();
    void onOutputChange();

    // Valid ≥2×2 selection left behind by cancelSelection (cleared on read).
    [[nodiscard]] std::optional<LogicalRect> takeAbandonedRegion();

    [[nodiscard]] bool isActive() const noexcept { return m_active; }
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

  private:
    struct Instance;

    DragMode m_dragMode = DragMode::None;
    double m_dragAnchorX = 0.0;
    double m_dragAnchorY = 0.0;
    double m_moveOffsetX = 0.0;
    double m_moveOffsetY = 0.0;

    void ensureSurfaces();
    void destroySurfaces();
    [[nodiscard]] bool surfacesMatchOutputs() const;
    void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
    void abortWithError(const std::string& message);
    void updateSelectionVisuals();
    void completeSelection();
    void confirmPendingSelection(ConfirmAction action = ConfirmAction::None);
    void completeFullscreenPick(wl_output* output);
    [[nodiscard]] std::optional<LogicalRect> selectionRectIfValid() const;

    WaylandConnection* m_wayland = nullptr;
    RenderContext* m_renderContext = nullptr;
    CompleteCallback m_onComplete;
    FailureCallback m_onFailure;
    std::vector<std::unique_ptr<Instance>> m_instances;
    std::vector<FrozenScreenshot> m_frozenScreenshots;
    std::optional<LogicalRect> m_abandonedRegion;
    std::string m_copyKeybindLabel;
    std::string m_saveKeybindLabel;
    std::string m_cancelKeybindLabel;
    bool m_active = false;
    bool m_freezeScreen = false;
    bool m_fullscreenPick = false;
    bool m_confirmRegion = false;
    bool m_confirming = false;
    bool m_dragging = false;
    double m_startGlobalX = 0.0;
    double m_startGlobalY = 0.0;
    double m_currentGlobalX = 0.0;
    double m_currentGlobalY = 0.0;
  };

} // namespace capture
