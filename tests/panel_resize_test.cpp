#include "compositors/compositor_platform.h"
#include "shell/panel/panel_manager.h"
#include "shell/workspace_map/workspace_map_panel.h"
#include "test_check.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"

class PanelResizeTestAccess {
public:
  static void checkAttachedResizeEntry() {
    WaylandConnection wayland{WaylandConnection::ShellProtocols{}};
    CompositorPlatform platform(wayland);
    WorkspaceMapPanel map(platform);
    PanelManager manager;
    manager.m_surface = std::make_unique<LayerSurface>(wayland, LayerSurfaceConfig{});
    manager.m_layerSurface = static_cast<LayerSurface*>(manager.m_surface.get());
    manager.m_activePanel = &map;
    manager.m_attachedToBar = true;
    manager.m_islandPresentation = true;
    manager.m_regularIslandAttachment = true;
    manager.m_panelScreenRect = {100, 120, 800, 600};
    manager.m_panelVisualWidth = 800;
    manager.m_panelVisualHeight = 600;
    // Ordinary panels attached to an island bar must reach the resize path.
    manager.beginPanelResize(790, 590);
    TEST_CHECK(manager.m_resizeDrag.active);
    // Actual dynamic-island content and content-fit panels remain excluded.
    manager.m_resizeDrag.active = false;
    manager.m_regularIslandAttachment = false;
    manager.beginPanelResize(790, 590);
    TEST_CHECK(!manager.m_resizeDrag.active);
    manager.m_regularIslandAttachment = true;
    manager.m_contentFit.enabled = true;
    manager.beginPanelResize(790, 590);
    TEST_CHECK(!manager.m_resizeDrag.active);
  }
  static InputRect target(InputRect rect, bool left, bool top, float dx, float dy, int w = 1920, int h = 1080) {
    return PanelManager::resizeTarget(rect, left, top, dx, dy, w, h, 1);
  }
};

int main() {
  PanelResizeTestAccess::checkAttachedResizeEntry();
  const InputRect initial{100, 120, 800, 600};
  const auto grown = PanelResizeTestAccess::target(initial, false, false, 200, 100);
  TEST_CHECK(grown.x == 100 && grown.y == 120 && grown.width == 1000 && grown.height == 700);
  const auto anchored = PanelResizeTestAccess::target(initial, true, true, -60, -40);
  TEST_CHECK(anchored.x + anchored.width == 900 && anchored.y + anchored.height == 720);
  TEST_CHECK(anchored.width == 860 && anchored.height == 640);
  const auto capped = PanelResizeTestAccess::target(initial, false, false, 10000, 10000);
  TEST_CHECK(capped.x + capped.width == 1912 && capped.y + capped.height == 1072);
  const auto minimum = PanelResizeTestAccess::target(initial, false, false, -10000, -10000);
  TEST_CHECK(minimum.width == 400 && minimum.height == 280);
  // A small work area takes precedence over the preferred minimum size.
  const auto small = PanelResizeTestAccess::target({10, 10, 200, 150}, false, false, 500, 500, 320, 240);
  TEST_CHECK(small.width == 302 && small.height == 222);
}
