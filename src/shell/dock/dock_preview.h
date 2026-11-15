#pragma once

#include "capture/toplevel_thumbnail_capture.h"
#include "render/scene/input_dispatcher.h"
#include "shell/dock/dock_model.h"
#include "shell/wayland/wayland_toplevels.h"
#include "ui/popup_chrome.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CompositorPlatform;
class ConfigService;
class IconResolver;
class Node;
class PopupSurface;
class RenderContext;
class WindowSwitcherTile;
struct PointerEvent;
struct PopupSurfaceConfig;

namespace shell::dock {

  struct DockInstance;

  // One session for all docks. The owner stays alive until Dock detaches it.
  class DockPreview {
  public:
    using Dismiss = std::function<void()>;

    DockPreview(
        DockInstance& owner, CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
        IconResolver& icons, Dismiss dismiss
    );
    ~DockPreview();

    bool show(const DockItemModel& item, const InputArea& anchor);
    bool refresh();
    bool routePointer(const PointerEvent& event);

    DockInstance& owner() const { return m_owner; }
    const std::string& app() const { return m_item.idLower; }
    bool pointerInside() const { return m_pointerInside; }

  private:
    struct Snapshot {
      ToplevelInfo window;
      std::shared_ptr<const ScreencopyImage> image;
    };

    void prepareFrame();
    const Snapshot* findSnapshot(const ToplevelInfo& window) const;
    void captureNext();
    void cancelCaptures();
    void changePage(int delta);
    void queueWindowAction(const ToplevelInfo& window, bool close);
    PopupSurfaceConfig geometry();
    bool updateGeometry();

    DockInstance& m_owner;
    CompositorPlatform& m_platform;
    ConfigService& m_config;
    RenderContext& m_renderContext;
    IconResolver& m_icons;
    Dismiss m_dismiss;
    DockItemModel m_item;
    std::string m_iconPath;
    std::vector<ToplevelInfo> m_windows;
    std::unique_ptr<PopupSurface> m_surface;
    std::unique_ptr<Node> m_sceneRoot;
    InputDispatcher m_input;
    popup_chrome::Geometry m_chrome;
    std::optional<ActiveToplevel> m_active;
    std::optional<std::string> m_focusedId;
    ToplevelThumbnailCapture m_capture;
    std::optional<ToplevelInfo> m_captureWindow;
    std::deque<Snapshot> m_snapshots;
    std::vector<WindowSwitcherTile*> m_tiles;
    std::size_t m_page = 0;
    std::size_t m_pageSize = 1;
    float m_anchorMain = 0.0F;
    float m_cardWidth = 0.0F;
    float m_cardHeight = 0.0F;
    float m_scale = 1.0F;
    bool m_vertical = false;
    bool m_pointerInside = false;
    bool m_dirty = true;
    bool m_thumbnailDirty = false;
    std::uint64_t m_appGeneration = 0;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };

} // namespace shell::dock
