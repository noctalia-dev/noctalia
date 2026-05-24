#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "ui/controls/select_dropdown_popup.h"
#include "wayland/layer_surface.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Box;
class Button;
class ConfigService;
class HttpClient;
class InputArea;
class MprisService;
class PipeWireSpectrum;
class RenderContext;
class Select;
class SystemMonitorService;
class WaylandConnection;
class WeatherService;
struct KeyboardEvent;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;
struct wl_surface;

class DesktopWidgetsEditor {
public:
  DesktopWidgetsEditor() = default;

  void initialize(
      WaylandConnection& wayland, ConfigService* config, PipeWireSpectrum* pipewireSpectrum,
      const WeatherService* weather, RenderContext* renderContext, MprisService* mpris, HttpClient* httpClient,
      SystemMonitorService* sysmon
  );
  void setExitRequestedCallback(std::function<void()> callback);

  void open(const DesktopWidgetsSnapshot& snapshot);
  [[nodiscard]] const DesktopWidgetsSnapshot& snapshot() const noexcept { return m_snapshot; }
  [[nodiscard]] DesktopWidgetsSnapshot close();
  [[nodiscard]] bool isOpen() const noexcept;

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  void onOutputChange();
  void onSecondTick();
  void requestLayout();
  void requestRedraw();

  void applySettingChange(const std::string& key, WidgetSettingValue value);

private:
  enum class ScaleCorner : std::uint8_t {
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight,
  };

  enum class DragMode : std::uint8_t {
    None,
    Move,
    Scale,
    Rotate,
    ToolbarMove,
    InspectorMove,
  };

  struct EditorWidgetView {
    std::unique_ptr<DesktopWidget> widget;
    Node* transformNode = nullptr;
    InputArea* bodyArea = nullptr;
    float intrinsicWidth = 0.0f;
    float intrinsicHeight = 0.0f;
  };

  struct OverlaySurface {
    std::string outputName;
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    std::unique_ptr<Node> sceneRoot;
    bool sceneRebuildRequested = true;
    std::unordered_map<std::string, EditorWidgetView> views;
    Node* selectionFrameTransform = nullptr;
    Node* selectionBorderTransform = nullptr;
    Box* selectionBorder = nullptr;
    Box* selectionBorderShadow = nullptr;
    Box* rotationRing = nullptr;
    Box* rotationRingShadow = nullptr;
    InputArea* rotateArea = nullptr;
    std::array<Box*, 4> scaleHandles{};
    std::array<Box*, 4> scaleHandleShadows{};
    std::array<InputArea*, 4> scaleAreas{};
    Node* toolbar = nullptr;
    float toolbarX = 0.0f;
    float toolbarY = 0.0f;
    bool toolbarPositionInitialized = false;
    Node* inspector = nullptr;
    float inspectorX = 0.0f;
    float inspectorY = 0.0f;
    bool inspectorPositionInitialized = false;
    std::unique_ptr<SelectDropdownPopup> selectPopup;
    bool pointerInside = false;
  };

  struct DragState {
    DragMode mode = DragMode::None;
    std::string widgetId;
    float startSceneX = 0.0f;
    float startSceneY = 0.0f;
    DesktopWidgetState initialState;
    float intrinsicWidth = 0.0f;
    float intrinsicHeight = 0.0f;
    ScaleCorner scaleCorner = ScaleCorner::BottomRight;
    std::string surfaceOutputName;
    float initialToolbarX = 0.0f;
    float initialToolbarY = 0.0f;
    float initialInspectorX = 0.0f;
    float initialInspectorY = 0.0f;
    bool rebuildOnFinish = false;
  };

  void syncSurfaces();
  void createSurface(const WaylandOutput& output);
  void rebuildScene(OverlaySurface& surface);
  void prepareFrame(OverlaySurface& surface, bool needsUpdate, bool needsLayout);
  void applyViewState(EditorWidgetView& view, const DesktopWidgetState& state, bool refreshContent);
  void updateViewTransforms(const std::string* relayoutWidgetId = nullptr);
  void updateSelectionVisuals(OverlaySurface& surface);
  void addWidget(const std::string& outputName, const std::string& type);
  void removeSelectedWidget();
  void toggleSelectedWidgetEnabled();
  void sendSelectedWidgetToBack();
  void bringSelectedWidgetToFront();
  void startToolbarDrag(const std::string& outputName);
  void startInspectorDrag(const std::string& outputName);
  void clampToolbarPosition(OverlaySurface& surface, float toolbarWidth, float toolbarHeight);
  void clampInspectorPosition(OverlaySurface& surface, float inspectorWidth, float inspectorHeight);
  void buildInspector(OverlaySurface& surface, Node& root, const DesktopWidgetState& selectedState);
  void deferEditorMutation(std::function<void()> action);
  void requestExit();
  void startDrag(
      DragMode mode, const std::string& widgetId, bool rebuildOnFinish,
      ScaleCorner scaleCorner = ScaleCorner::BottomRight
  );
  void updateDrag();
  void finishDrag();
  [[nodiscard]] OverlaySurface* findSurface(wl_surface* surface);
  [[nodiscard]] OverlaySurface* findSurface(const std::string& outputName);
  [[nodiscard]] OverlaySurface* findSurfaceForWidget(const std::string& widgetId);
  [[nodiscard]] EditorWidgetView* findView(const std::string& id);
  [[nodiscard]] DesktopWidgetState* findWidgetState(const std::string& id);
  [[nodiscard]] const DesktopWidgetState* findWidgetState(const std::string& id) const;
  [[nodiscard]] std::string effectiveOutputName(const DesktopWidgetState& state) const;
  [[nodiscard]] bool shouldSnap() const;
  [[nodiscard]] float widgetContentScale(const DesktopWidgetState& state) const;

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  std::string m_addWidgetType = "clock";
  std::function<void()> m_exitRequestedCallback;
  DesktopWidgetsSnapshot m_snapshot;
  std::vector<std::unique_ptr<OverlaySurface>> m_surfaces;
  std::string m_selectedWidgetId;
  DragState m_drag;
  bool m_open = false;
  bool m_shiftHeld = false;
  bool m_leftShiftHeld = false;
  bool m_rightShiftHeld = false;
  bool m_altHeld = false;
  bool m_leftAltHeld = false;
  bool m_rightAltHeld = false;
  float m_currentEventSceneX = 0.0f;
  float m_currentEventSceneY = 0.0f;
  bool m_inspectorOpen = false;
};
