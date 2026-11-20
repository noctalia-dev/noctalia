#pragma once

#include "launcher/launcher_provider.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_resolver.h"
#include "wayland/wayland_seat.h"

#include <array>
#include <memory>
#include <vector>

class AppProvider;
class Image;
class Glyph;
class Button;
class CompositorPlatform;
class ConfigService;
class Input;
class Label;
class LayerSurface;
class Node;
class PanelManager;
class RenderContext;
class WaylandConnection;
struct wl_output;

// A shell-owned search field above Umbriel's live spread. Keyboard focus is
// used for typing; empty-query navigation is handed back to the compositor.
class OverviewAppSearch {
public:
  OverviewAppSearch();
  ~OverviewAppSearch();
  void initialize(
      WaylandConnection& wayland, RenderContext& renderContext, CompositorPlatform& platform, ConfigService& config,
      PanelManager& panels
  );
  void sync();
  void rebuild();
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  void destroy();
  void create(wl_output* output, int outputWidth, int outputHeight);
  void prepareFrame();
  void refreshResults();
  void activate(std::size_t index);
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  PanelManager* m_panels = nullptr;
  std::unique_ptr<AppProvider> m_apps;
  IconResolver m_iconResolver;
  bool m_typeToSearch = false;
  std::unique_ptr<LayerSurface> m_surface;
  std::unique_ptr<Node> m_scene;
  InputDispatcher m_dispatcher;
  Input* m_input = nullptr;
  Node* m_resultsBox = nullptr;
  Label* m_empty = nullptr;
  std::array<Button*, 6> m_buttons{};
  std::array<Image*, 6> m_images{};
  std::array<Glyph*, 6> m_fallbackGlyphs{};
  std::vector<LauncherResult> m_results;
  std::size_t m_selected = 0;
  float m_scale = 1.0F;
  std::uint32_t m_requestedHeight = 0;
  // Deferred activation must not outlive this search session or the host.
  std::shared_ptr<void> m_session;
};
