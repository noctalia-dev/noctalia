#include "filemanager/dialogs/progress_window.h"

#include "core/files/display_text.h"
#include "core/ui_phase.h"
#include "filemanager/file_operation_manager.h"
#include "render/render_context.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <filesystem>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <sstream>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace filemanager {

  namespace {
    void applyNytheraBevel(Flex& panel, float radius) {
      panel.setRadius(radius);
      panel.setCornerShapes(
          CornerShapes{
              .tl = CornerShape::Chamfered,
              .tr = CornerShape::Chamfered,
              .br = CornerShape::Chamfered,
              .bl = CornerShape::Chamfered,
          }
      );
    }

    std::string progressSummary(const std::vector<FileOperationManager::Operation>& operations) {
      std::ostringstream summary;
      std::size_t active = 0;
      for (const auto& operation : operations)
        active += operation.finished ? 0U : 1U;
      summary << active << (active == 1 ? " active operation" : " active operations");
      std::size_t shown = 0;
      for (const auto& operation : operations) {
        if (operation.finished)
          continue;
        if (shown == 4) {
          summary << "\n…and " << (active - shown) << " more active operations";
          break;
        }
        const auto name = std::filesystem::path(operation.source.empty() ? operation.destination : operation.source)
                              .filename()
                              .string();
        const auto displayName = name.empty() ? std::string("Working") : sanitizeFilenameForDisplay(name);
        summary << "\n" << displayName << " — " << operation.status;
        if (operation.fraction > 0.0)
          summary << " (" << static_cast<int>(operation.fraction * 100.0) << "%)";
        ++shown;
      }
      return summary.str();
    }
  } // namespace

  ProgressWindow::ProgressWindow(
      WaylandConnection& wayland, RenderContext& renderContext, FileOperationManager& operations,
      std::function<void(ToplevelSurface*)> onRequestQuit
  )
      : m_wayland(wayland),
        m_renderContext(renderContext),
        m_operations(operations),
        m_onRequestQuit(std::move(onRequestQuit)) {
    m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_wayland.setCursorShape(serial, shape);
    });
  }

  ProgressWindow::~ProgressWindow() {
    *m_alive = false;
    close();
  }

  void ProgressWindow::close() {
    m_wayland.stopKeyRepeat();
    if (m_surface) {
      m_dispatcher.setTextInputContext(nullptr, nullptr);
      m_dispatcher.setSceneRoot(nullptr);
      m_surface->setSceneRoot(nullptr);
      m_surface.reset();
    }
    m_root.reset();
    m_label = nullptr;
    m_cancelButton = nullptr;
  }

  void ProgressWindow::sync(ToplevelSurface* parent, bool backgroundConflictPending) {
    m_lastParent = parent;
    m_backgroundConflictPending = backgroundConflictPending;
    if (!m_wayland.isConnected())
      return;
    const bool visibleJob = std::ranges::any_of(m_operations.operations(), [](const auto& operation) {
      return !operation.finished && operation.progressRevealed;
    });
    const bool needFallback = visibleJob && (!m_operations.hasJobService() || backgroundConflictPending);
    if (!needFallback) {
      close();
      return;
    }
    if (m_surface) {
      if (m_label != nullptr)
        m_label->setText(progressSummary(m_operations.operations()));
      m_surface->requestLayout();
      return;
    }
    auto surface = std::make_unique<ToplevelSurface>(m_wayland);
    surface->setRenderContext(&m_renderContext);
    surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
      if (m_surface)
        m_surface->requestLayout();
    });
    surface->setClosedCallback([this] {
      struct ProgressCloseRequest {
        ProgressWindow* self;
        std::weak_ptr<bool> alive;
      };
      g_idle_add(
          [](gpointer context) -> gboolean {
            std::unique_ptr<ProgressCloseRequest> request(static_cast<ProgressCloseRequest*>(context));
            const auto locked = request->alive.lock();
            if (!locked || !*locked)
              return G_SOURCE_REMOVE;
            auto* self = request->self;
            self->close();
            self->sync(self->m_lastParent, self->m_backgroundConflictPending);
            return G_SOURCE_REMOVE;
          },
          new ProgressCloseRequest{this, m_alive}
      );
    });
    surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
    ToplevelSurfaceConfig config{
        .width = 560,
        .height = 340,
        .minWidth = 480,
        .minHeight = 260,
        .maxWidth = 640,
        .maxHeight = 440,
        .parent = parent ? parent->xdgToplevel() : nullptr,
        .title = "Nythera Files — Operations",
        .appId = "dev.nythera.FileManager.Progress"
    };
    wl_output* output = m_wayland.outputs().empty() ? nullptr : m_wayland.outputs().front().output;
    if (!surface->initialize(output, config))
      return;
    m_wayland.stopKeyRepeat();
    m_surface = std::move(surface);
    m_closed = false;
  }

  void ProgressWindow::prepareFrame(bool, bool needsLayout) {
    if (!m_surface || m_surface->width() == 0 || m_surface->height() == 0)
      return;
    m_renderContext.makeCurrent(m_surface->renderTarget());
    auto& renderer = m_surface->renderTarget().renderer();
    if (!m_root) {
      m_root = std::make_unique<Flex>();
      m_root->setDirection(FlexDirection::Vertical);
      m_root->setAlign(FlexAlign::Stretch);
      m_root->setGap(8.0F);
      m_root->setPadding(16.0F);
      m_root->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow, 0.95F));
      m_root->setBorder(colorSpecFromRole(ColorRole::Primary, 0.55F), Style::borderWidth);
      applyNytheraBevel(*m_root, Style::scaledRadiusMd(1.0F));
      auto heading = std::make_unique<Label>();
      heading->setText("FILE OPERATIONS");
      heading->setFontWeight(FontWeight::SemiBold);
      heading->setColor(colorSpecFromRole(ColorRole::Primary));
      m_root->addChild(std::move(heading));
      auto label = std::make_unique<Label>();
      label->setText(progressSummary(m_operations.operations()));
      label->setMaxWidth(600.0F);
      label->setMaxLines(12);
      label->setFlexGrow(1.0F);
      m_label = label.get();
      m_root->addChild(std::move(label));
      auto cancel = std::make_unique<Button>();
      cancel->setText("Cancel active operations");
      cancel->setVariant(ButtonVariant::Outline);
      cancel->setOnClick([this] {
        for (const auto& operation : m_operations.operations())
          if (!operation.finished)
            m_operations.cancel(operation.id);
      });
      m_cancelButton = cancel.get();
      m_root->addChild(std::move(cancel));
      auto quit = std::make_unique<Button>();
      quit->setText("Quit and stop operations…");
      quit->setVariant(ButtonVariant::Ghost);
      quit->setOnClick([this] {
        if (m_onRequestQuit)
          m_onRequestQuit(m_surface.get());
      });
      m_root->addChild(std::move(quit));
      m_dispatcher.setSceneRoot(m_root.get());
      m_dispatcher.setFocus(m_cancelButton->inputArea());
      m_surface->setSceneRoot(m_root.get());
      needsLayout = true;
    }
    if (m_label != nullptr)
      m_label->setText(progressSummary(m_operations.operations()));
    if (needsLayout) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
      m_root->layout(renderer);
    }
  }

  void ProgressWindow::handlePointer(const PointerEvent& event) {
    const float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_dispatcher.pointerEnter(x, y, event.serial);
      break;
    case PointerEvent::Type::Leave:
      m_dispatcher.pointerLeave();
      break;
    case PointerEvent::Type::Motion:
      m_dispatcher.pointerMotion(x, y, event.serial);
      break;
    case PointerEvent::Type::Button:
      if (event.button == BTN_LEFT)
        m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, false);
      break;
    case PointerEvent::Type::Axis:
      m_dispatcher.pointerAxis(
          x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
          event.axisGestureSerial
      );
      break;
    }
  }

  void ProgressWindow::handleKeyboard(const KeyboardEvent& event) {
    if (!m_surface || m_wayland.lastKeyboardSurface() != m_surface->wlSurface())
      return;
    if (!event.pressed) {
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, false, event.preedit, false);
      return;
    }
    if (event.preedit) {
      if (!event.repeat)
        m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, true, true, false);
      return;
    }
    if (!event.repeat && event.modifiers == 0U && (event.sym == XKB_KEY_Escape || event.sym == XKB_KEY_Delete)) {
      m_wayland.stopKeyRepeat();
      for (const auto& operation : m_operations.operations())
        if (!operation.finished)
          m_operations.cancel(operation.id);
      return;
    }
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit, event.repeat);
  }

} // namespace filemanager
