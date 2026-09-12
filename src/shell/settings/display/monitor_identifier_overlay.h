#pragma once

#include <memory>
#include <vector>

class LayerSurface;
class Node;
class RenderContext;
class WaylandConnection;
struct wl_output;

namespace settings::display {

  // Per-output layer-surface badges ("N <connector>") shown while the monitor
  // editor is open so drag targets are identifiable on the physical screens.
  class MonitorIdentifierOverlay {
  public:
    MonitorIdentifierOverlay();
    ~MonitorIdentifierOverlay();

    void initialize(WaylandConnection& wayland, RenderContext* renderContext);
    void show();
    void hide();
    void onOutputChange();
    [[nodiscard]] bool visible() const { return m_visible; }

  private:
    struct Instance {
      wl_output* output = nullptr;
      std::unique_ptr<LayerSurface> surface;
      std::unique_ptr<Node> sceneRoot;
      int scale = 1;
    };

    void ensureSurfaces();
    void destroySurfaces();
    void buildScene(Instance& instance);
    void prepareFrame(Instance& instance, bool needsUpdate, bool needsLayout);

    WaylandConnection* m_wayland = nullptr;
    RenderContext* m_renderContext = nullptr;
    std::vector<std::unique_ptr<Instance>> m_instances;
    bool m_visible = false;
    std::string m_signature;
  };

} // namespace settings::display
