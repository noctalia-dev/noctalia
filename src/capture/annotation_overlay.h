#pragma once

#include "capture/annotation_document.h"
#include "capture/screenshot_backdrop.h"
#include "capture/screenshot_image.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Flex;
class Renderer;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;

namespace capture {

  enum class AnnotationExport : std::uint8_t {
    Copy,
    Save,
    // Image mode only: deliver with the policy the plain screenshot would have used.
    Done,
  };

  enum class AnnotationMode : std::uint8_t {
    // Transparent overlay over the running desktop; Freeze captures the background.
    Live,
    // The whole desktop frozen per output.
    Frozen,
    // A single already-captured image, shown centered on one output.
    Image,
  };

  enum class AnnotationFillMode : std::uint8_t {
    Off,
    Semi,
    Opaque,
  };

  struct AnnotationToolState {
    AnnotationTool tool = AnnotationTool::Brush;
    std::array<AnnotationColor, kAnnotationToolCount> color{};
    std::array<double, kAnnotationToolCount> width{};
    AnnotationFillMode fill = AnnotationFillMode::Off;
    // Each output's dragged toolbar spot survives across runs.
    std::unordered_map<std::string, AnnotationPoint> toolbarPositions;
  };

  [[nodiscard]] AnnotationToolState defaultAnnotationToolState();

  [[nodiscard]] std::string_view annotationFillModeName(AnnotationFillMode mode) noexcept;
  [[nodiscard]] std::optional<AnnotationFillMode> annotationFillModeFromName(std::string_view name) noexcept;

  class AnnotationOverlay {
  public:
    using ExportCallback = std::function<void(ScreencopyImage image, AnnotationExport action)>;
    using FreezeCallback = std::function<void()>;
    using CaptureRegionCallback = std::function<void()>;
    using ClosedCallback = std::function<void()>;
    using FailureCallback = std::function<void(const std::string& message)>;
    using FeedbackCallback = std::function<void(const std::string& message)>;
    using StateSetter = std::function<void(std::string_view key, std::string_view value)>;
    using BackdropConfig = ScreenshotBackdropSettings;
    // Fired once when the editor closes, if the backdrop panel changed anything.
    using BackdropChangedCallback = std::function<void(const BackdropConfig& backdrop)>;

    AnnotationOverlay();
    ~AnnotationOverlay();

    void initialize(WaylandConnection& wayland, RenderContext* renderContext);
    void setExportCallback(ExportCallback callback);
    void setFreezeCallback(FreezeCallback callback);
    void setCaptureRegionCallback(CaptureRegionCallback callback);
    void setClosedCallback(ClosedCallback callback);
    void setFailureCallback(FailureCallback callback);
    void setFeedbackCallback(FeedbackCallback callback);
    void setStateSetter(StateSetter setter);
    void setBackdropChangedCallback(BackdropChangedCallback callback);

    // Image mode previews this backdrop around the capture and edits it from the backdrop panel; it is the
    // only source of the screenshot backdrop. Set it before beginImage(); the canvas reserves room for
    // the padding it starts with.
    void setScreenshotBackdrop(const BackdropConfig& backdrop);
    [[nodiscard]] const BackdropConfig& screenshotBackdrop() const noexcept { return m_backdrop; }

    void setToolState(const AnnotationToolState& state);
    [[nodiscard]] const AnnotationToolState& toolState() const noexcept { return m_tools; }

    // Empty vector selects Live, a non-empty one Frozen.
    void setFrozenScreenshots(std::vector<FrozenScreenshot> frozen);
    void setCursorVisible(bool visible);
    [[nodiscard]] bool cursorVisible() const noexcept;
    void setCopyOnClose(bool copy) noexcept { m_copyOnClose = copy; }

    void begin();
    void beginImage(ScreenshotImage image, wl_output* target);
    // Drops the surfaces so a screencopy frame cannot contain the overlay; documents survive.
    void hideForCapture();
    void resumeAfterCapture();
    void cancel();
    void onOutputChange();

    [[nodiscard]] bool isActive() const noexcept { return m_active; }
    [[nodiscard]] AnnotationMode mode() const noexcept { return m_mode; }
    [[nodiscard]] bool onPointerEvent(const PointerEvent& event);
    [[nodiscard]] bool onKeyboardEvent(const KeyboardEvent& event);

  private:
    struct Instance;

    void ensureSurfaces();
    void destroySurfaces();
    [[nodiscard]] bool surfacesMatchOutputs() const;
    void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
    void buildScene(Instance& instance);
    [[nodiscard]] std::unique_ptr<Flex> buildToolbar(Instance& instance);
    [[nodiscard]] std::unique_ptr<Flex> buildBackdropPanel(Instance& instance);
    void refreshBackdropPanel(Instance& instance, Renderer* renderer);
    void positionBackdropPanel(Instance& instance);
    void updateBackdropPreview(Instance& instance, Renderer& renderer);
    void toggleBackdropPanel();
    void editBackdrop(const std::function<void(BackdropConfig&)>& edit);
    [[nodiscard]] bool backdropPanelInputFocused(const Instance& instance) const;
    [[nodiscard]] std::unique_ptr<Flex> buildStrokePanel(Instance& instance);
    void refreshStrokePanel(Instance& instance, Renderer* renderer);
    void positionStrokePanel(Instance& instance);
    void toggleStrokePanel();
    void refreshToolbar(Instance& instance, Renderer* renderer);
    void positionToolbar(Instance& instance);
    void syncToolbarPositionsForConnectedOutputs();
    void beginToolbarDrag(Instance& instance);
    void dragToolbarTo(double surfaceX, double surfaceY);
    [[nodiscard]] std::size_t nearestSizePreset(AnnotationTool tool) const;
    void applySizePreset(std::size_t index);
    void layoutCanvas(Instance& instance);
    void redrawCommitted(Instance& instance);
    void redrawActive(Instance& instance, std::optional<AnnotationRect> changedBounds = std::nullopt);
    void queueActiveRedraw(Instance& instance, std::optional<AnnotationRect> changedBounds);
    void uploadDirty(Instance& instance, Renderer& renderer);
    void foldIntoCommitted(Instance& instance, const Annotation& annotation);
    void abortWithError(const std::string& message);

    [[nodiscard]] const ScreenshotImage* screenshot(const Instance& instance) const;
    [[nodiscard]] const char* cursorTooltipKey(const Instance& instance) const;
    [[nodiscard]] const ScreencopyImage* background(const Instance& instance) const;
    [[nodiscard]] AnnotationDocument& documentFor(const Instance& instance);
    [[nodiscard]] Instance* instanceForSurface(const void* surface);
    [[nodiscard]] Instance* exportTarget();
    void requestRedrawAll();
    void requestFullRefresh();
    void markCommittedFullRedraw();

    void onCanvasPress(Instance& instance, double x, double y, bool pressed);
    void onCanvasMotion(Instance& instance, double x, double y);
    void finishGesture();
    void commitTextEdit();
    void cancelTextEdit();
    void commitPendingText();
    void restyleActiveText();
    [[nodiscard]] bool handleTextKey(const KeyboardEvent& event);
    [[nodiscard]] bool handleToolKey(std::uint32_t sym);

    void selectTool(AnnotationTool tool);
    void setToolColor(AnnotationColor color);
    void adjustToolWidth(double delta);
    void cycleFill(bool forward = true);
    void undo();
    void redo();
    void requestExport(AnnotationExport action);
    void requestFreeze();
    void requestCaptureRegion();
    void toggleCursor(Instance* instance = nullptr);
    void requestClose();
    void closeOverlay();
    void persistToolState();

    void markActiveDirty(Instance& instance, const AnnotationRect& logicalBounds);
    void markCommittedDirty(Instance& instance, const AnnotationRect& logicalBounds);
    void updateCropVisuals(Instance& instance);

    WaylandConnection* m_wayland = nullptr;
    RenderContext* m_renderContext = nullptr;
    ExportCallback m_onExport;
    FreezeCallback m_onFreeze;
    CaptureRegionCallback m_onCaptureRegion;
    ClosedCallback m_onClosed;
    FailureCallback m_onFailure;
    FeedbackCallback m_onFeedback;
    StateSetter m_stateSetter;
    BackdropChangedCallback m_onBackdropChanged;

    std::vector<std::unique_ptr<Instance>> m_instances;
    std::unordered_map<wl_output*, AnnotationDocument> m_documents;
    std::vector<FrozenScreenshot> m_frozen;
    ScreenshotImage m_image;
    wl_output* m_imageOutput = nullptr;

    AnnotationMode m_mode = AnnotationMode::Live;
    AnnotationToolState m_tools = defaultAnnotationToolState();
    bool m_active = false;
    bool m_cursorVisible = false;
    bool m_copyOnClose = false;

    // One gesture at a time, owned by the instance the press landed on.
    Instance* m_gestureInstance = nullptr;
    bool m_gestureActive = false;
    AnnotationPoint m_gestureStart{};
    AnnotationPoint m_gestureLast{};
    std::optional<std::size_t> m_moveIndex;
    bool m_moved = false;
    bool m_cropDragging = false;
    AnnotationPoint m_cropStart{};

    Instance* m_textInstance = nullptr;
    std::optional<std::size_t> m_textIndex;

    // Last surface-local pointer position, the anchor the toolbar drag works from.
    double m_pointerX = 0.0;
    double m_pointerY = 0.0;

    // Runtime positions use wl_output identity so unnamed outputs remain independent while active.
    std::unordered_map<wl_output*, AnnotationPoint> m_toolbarPositions;
    Instance* m_toolbarDragInstance = nullptr;
    float m_toolbarGrabX = 0.0F;
    float m_toolbarGrabY = 0.0F;
    bool m_strokePanelOpen = false;
    bool m_syncingSizePreset = false;

    BackdropConfig m_backdrop;
    bool m_backdropEdited = false;
    bool m_backdropPanelOpen = false;
    // Padding the Image-mode canvas was fitted around. Fixed for the session because the document
    // stores annotations in canvas coordinates, so the canvas must not resize under them.
    int m_backdropLayoutPadding = 0;

    std::vector<std::uint8_t> m_uploadScratch;
  };

} // namespace capture
