#include "core/deferred_call.h"
#include "render/render_context.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/wayland_connection.h"

#include <memory>

class ContextMenuPopupTestAccess {
public:
  static void activate(ContextMenuPopup& popup, ContextMenuControlEntry entry) {
    popup.deferActivation(std::move(entry));
  }

  static void dismiss(ContextMenuPopup& popup) { popup.deferClose(); }
};

namespace {

  class PopupOwningPanel final : public Panel {
  public:
    PopupOwningPanel(WaylandConnection& wayland, RenderContext& renderContext, int& activations)
        : m_popup(wayland, renderContext) {
      m_popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    }

    void create() override {}
    [[nodiscard]] float preferredWidth() const override { return 1.0F; }
    [[nodiscard]] float preferredHeight() const override { return 1.0F; }

    void deferActivation() {
      ContextMenuPopupTestAccess::activate(m_popup, ContextMenuControlEntry{.id = 2, .label = "Delete"});
    }

  protected:
    void doLayout(Renderer&, float, float) override {}

  private:
    ContextMenuPopup m_popup;
  };

  void drainDeferredCalls() {
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

} // namespace

int main() {
  WaylandConnection wayland;
  RenderContext renderContext;

  int activations = 0;
  {
    ContextMenuPopup popup(wayland, renderContext);
    popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    ContextMenuPopupTestAccess::activate(popup, ContextMenuControlEntry{.id = 1, .label = "Copy"});
    drainDeferredCalls();
  }
  TEST_CHECK(activations == 1);

  // Plugin unregistration destroys its panel-owned popup immediately. A queued
  // activation must then become a no-op instead of dereferencing the destroyed
  // ContextMenuPopup on the next main-loop iteration.
  {
    PanelManager panels;
    auto panel = std::make_unique<PopupOwningPanel>(wayland, renderContext, activations);
    panel->deferActivation();
    panels.registerPanel("test/plugin:panel", std::move(panel));
    panels.unregisterPanel("test/plugin:panel");
  }
  drainDeferredCalls();
  TEST_CHECK(activations == 1);

  return 0;
}
