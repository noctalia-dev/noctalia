#pragma once

#include "render/animation/animation_manager.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ConfigService;
class Box;
class Flex;
class Glyph;
class Label;
class ProgressBar;
class RenderContext;
class WaylandConnection;
struct WaylandOutput;
struct wl_surface;

enum class OsdKind : std::uint8_t {
  Volume,
  Microphone,
  Brightness,
  Wifi,
  Bluetooth,
  PowerProfile,
  Caffeine,
  NightLight,
  Dnd,
  LockKeys,
  KeyboardLayout,
  Media,
  Privacy,
  KeyboardBacklight
};

struct OsdContent {
  OsdKind kind = OsdKind::Volume;
  std::string icon;
  std::string value;
  float progress = 0.0f;
  bool showProgress = true;
  bool overLimit = false;
  bool inactive = false;
};

class OsdOverlay {
public:
  OsdOverlay() = default;
  ~OsdOverlay() = default;

  OsdOverlay(const OsdOverlay&) = delete;
  OsdOverlay& operator=(const OsdOverlay&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onOutputChange();
  void onConfigReload();
  void requestLayout();
  void requestRedraw();

  void show(const OsdContent& content);

  // True while any instance is on screen or animating into view. Callers use this to correct
  // already-visible content in place without popping up a fresh OSD.
  [[nodiscard]] bool isVisible() const;

private:
  struct SurfaceMargins {
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::int32_t left = 0;
  };

  struct Instance {
    wl_output* output = nullptr;
    std::int32_t scale = 1;
    float uiLayoutScale = 1.0f;
    std::unique_ptr<LayerSurface> surface;
    // sceneRoot must be destroyed before `animations` — ~Node() calls cancelForOwner().
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    wl_surface* wlSurface = nullptr;

    Node* card = nullptr;
    Flex* row = nullptr;
    Box* background = nullptr;
    Glyph* glyph = nullptr;
    Label* value = nullptr;
    ProgressBar* progress = nullptr;
    float progressValueMinWidth = 0.0f;
    float rowBaseX = 0.0f;
    float rowBaseY = 0.0f;
    AnimationManager::Id showAnimId = 0;
    AnimationManager::Id hideAnimId = 0;
    bool showPending = false;
    bool visible = false;
    float appliedCornerRadiusScale = -1.0f;
  };

  [[nodiscard]] SurfaceMargins surfaceMarginsForPosition(const std::string& position) const;
  [[nodiscard]] std::vector<std::string> osdMonitors() const;
  [[nodiscard]] bool shouldRenderOnOutput(const WaylandOutput& output) const;
  void ensureSurfaces();
  void destroySurfaces();
  void prepareFrame(Instance& inst, bool needsUpdate, bool needsLayout);
  void buildScene(Instance& inst, std::uint32_t width, std::uint32_t height);
  void updateInstanceContent(Instance& inst);
  void updateBlurRegion(Instance& inst) const;
  void applyReveal(Instance& inst, float reveal);
  void animateInstance(Instance& inst);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  OsdContent m_content;
  std::string m_lastPosition;
  std::string m_lastOrientation;
  bool m_lastShowProgress = true;
  float m_lastLayoutScale = 1.0f;
  float m_lastCornerRadiusScale = 1.0f;
  std::vector<std::string> m_lastMonitorSelectors;
  std::vector<std::unique_ptr<Instance>> m_instances;
};
