#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "wayland/layer_surface.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class CompositorPlatform;
class ConfigService;
class Node;
class Panel;
class RenderContext;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

// Hosts panels that live outside PanelManager's single active-panel slot: opening
// another panel does not close them, and they are dismissed only by an explicit
// toggle or close. Each one owns an independent layer surface with its own scene,
// animations and input dispatch, so several can be open at once.
//
// The feature set is deliberately narrower than PanelManager's: floating placement
// only (screen position or centered), no attached-to-bar reveal, and no outside
// click dismissal — a persistent panel is meant to sit alongside the app the user
// is working in rather than take over from it.
class PersistentPanelHost {
public:
  PersistentPanelHost();
  ~PersistentPanelHost();

  PersistentPanelHost(const PersistentPanelHost&) = delete;
  PersistentPanelHost& operator=(const PersistentPanelHost&) = delete;

  void initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);

  void registerPanel(const std::string& id, std::unique_ptr<Panel> content);
  void unregisterPanel(const std::string& id);
  [[nodiscard]] bool hasPanel(std::string_view id) const noexcept;
  void appendPanelIds(std::vector<std::string>& out) const;

  void open(const std::string& id, wl_output* output, std::string_view context);
  void close(const std::string& id);
  void toggle(const std::string& id, wl_output* output, std::string_view context);
  void closeAll();
  [[nodiscard]] bool isOpen(std::string_view id) const noexcept;
  [[nodiscard]] bool anyOpen() const noexcept { return !m_instances.empty(); }

  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

  void refreshPanel(std::string_view id);
  void onConfigReloaded();
  void onIconThemeChanged();
  void refresh();

private:
  struct Instance {
    std::string id;
    Panel* panel = nullptr;
    wl_output* output = nullptr;
    // Declared before sceneRoot: ~Node() calls cancelForOwner() on the manager, so
    // the scene (and the panel nodes parented into it) must die first.
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> sceneRoot;
    Node* contentNode = nullptr;
    Box* bgNode = nullptr;
    Box* shadowNode = nullptr;
    std::int32_t insetX = 0;
    std::int32_t insetY = 0;
    std::int32_t bleedRight = 0;
    std::int32_t bleedBottom = 0;
    std::uint32_t visualWidth = 0;
    std::uint32_t visualHeight = 0;
    bool fillWidth = false;
    bool fillHeight = false;
    bool pointerInside = false;
  };

  void buildScene(Instance& instance, std::uint32_t width, std::uint32_t height);
  void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);
  void layoutScene(Instance& instance, std::uint32_t width, std::uint32_t height);
  [[nodiscard]] Instance* findInstance(std::string_view id) noexcept;
  [[nodiscard]] Instance* findInstanceForSurface(wl_surface* surface) noexcept;
  void destroyInstance(std::vector<std::unique_ptr<Instance>>::iterator it);

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::unordered_map<std::string, std::unique_ptr<Panel>> m_panels;
  std::vector<std::unique_ptr<Instance>> m_instances;
};
