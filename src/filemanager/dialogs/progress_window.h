#pragma once

#include "render/scene/input_dispatcher.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_seat.h"

#include <functional>
#include <memory>

class Button;
class FileOperationManager;
class Flex;
class Label;
class RenderContext;
class WaylandConnection;

namespace filemanager {

  class ProgressWindow {
  public:
    ProgressWindow(
        WaylandConnection& wayland, RenderContext& renderContext, FileOperationManager& operations,
        std::function<void(ToplevelSurface*)> onRequestQuit
    );
    ~ProgressWindow();

    void sync(ToplevelSurface* parent, bool backgroundConflictPending);
    void close();
    void handlePointer(const PointerEvent& event);
    void handleKeyboard(const KeyboardEvent& event);

    [[nodiscard]] bool isOpen() const noexcept { return m_surface != nullptr; }
    [[nodiscard]] ToplevelSurface* surface() const noexcept { return m_surface.get(); }
    [[nodiscard]] wl_surface* wlSurface() const noexcept { return m_surface ? m_surface->wlSurface() : nullptr; }
    [[nodiscard]] bool ownsSurface(wl_surface* surface) const noexcept {
      return m_surface && surface == m_surface->wlSurface();
    }

  private:
    void prepareFrame(bool needsUpdate, bool needsLayout);

    WaylandConnection& m_wayland;
    RenderContext& m_renderContext;
    FileOperationManager& m_operations;
    std::function<void(ToplevelSurface*)> m_onRequestQuit;
    std::unique_ptr<ToplevelSurface> m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Flex> m_root;
    Label* m_label = nullptr;
    Button* m_cancelButton = nullptr;
    bool m_closed = false;
    bool m_backgroundConflictPending = false;
    ToplevelSurface* m_lastParent = nullptr;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  };

} // namespace filemanager
