#include "core/deferred_call.h"
#include "render/render_context.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu_geometry.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/wayland_connection.h"

#include <memory>

class ContextMenuPopupTestAccess {
public:
  static void activate(ContextMenuPopup& popup, ContextMenuControlEntry entry) {
    popup.deferActivation(std::move(entry));
  }

  static void dismiss(ContextMenuPopup& popup) { popup.deferClose(); }

  static void queueNestedLeaf(ContextMenuPopup& parent, int& activations) {
    parent.m_child = std::make_unique<ContextMenuPopup>(parent.m_wayland, parent.m_renderContext);
    parent.m_child->m_parentPopup = &parent;
    parent.m_child->setOnActivate([&parent, &activations](const ContextMenuControlEntry&) {
      parent.close();
      ++activations;
    });
    parent.m_child->deferActivation(ContextMenuControlEntry{.id = 3, .label = "Nested leaf"});
  }

  static bool hasChild(const ContextMenuPopup& popup) { return popup.m_child != nullptr; }
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
  const auto shortHint = context_menu_geometry::columns(240, 8, 8, 44, 12);
  const auto longHint = context_menu_geometry::columns(240, 8, 8, 96, 12);
  TEST_CHECK(shortHint.shortcutX + shortHint.shortcutWidth == 232);
  TEST_CHECK(longHint.shortcutX + longHint.shortcutWidth == 232);
  TEST_CHECK(longHint.shortcutWidth == 96); // No arbitrary shortcut-column cap.
  TEST_CHECK(8 + longHint.labelWidth + 12 == longHint.shortcutX);
  const auto submenuHint = context_menu_geometry::columns(240, 24, 30, 96, 12);
  TEST_CHECK(submenuHint.shortcutX + submenuHint.shortcutWidth == 210);
  const auto narrow = context_menu_geometry::columns(50, 8, 8, 96, 12);
  TEST_CHECK(narrow.labelWidth == 0 && narrow.shortcutWidth == 34 && narrow.shortcutX == 8);

  TEST_CHECK(context_menu_geometry::visibleRows({24.0F, 24.0F, 10.0F, 24.0F}, 4, 60.0F, 4.0F) == 2);
  TEST_CHECK(context_menu_geometry::visibleRows({36.0F, 15.0F, 36.0F}, 3, 100.0F, 8.0F) == 2);
  TEST_CHECK(context_menu_geometry::visibleRows({10000.0F, 10000.0F}, 2, 20000.0F, 0.0F) == 2);
  TEST_CHECK(context_menu_geometry::visibleRows({20.0F, 20.0F, 20.0F}, 3, 58.0F, 4.0F, 2.0F) == 2);
  // Preserve full height on a tall output, including an exact fit with shadow.
  TEST_CHECK(context_menu_geometry::fitHeight(700.0F, 1080.0F, 40.0F) == 700.0F);
  TEST_CHECK(context_menu_geometry::fitHeight(700.0F, 740.0F, 40.0F) == 700.0F);
  // A smaller compositor configure uses the whole remaining viewport.
  TEST_CHECK(context_menu_geometry::fitHeight(700.0F, 503.0F, 40.0F) == 463.0F);
  TEST_CHECK(context_menu_geometry::fitHeight(700.0F, 28.0F, 40.0F) == 1.0F);

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

  {
    ContextMenuPopup parent(wayland, renderContext);
    ContextMenuPopupTestAccess::queueNestedLeaf(parent, activations);
    drainDeferredCalls();
    TEST_CHECK(activations == 2);
    TEST_CHECK(!ContextMenuPopupTestAccess::hasChild(parent));
  }
  {
    ContextMenuPopup parent(wayland, renderContext);
    ContextMenuPopupTestAccess::queueNestedLeaf(parent, activations);
  }
  drainDeferredCalls();
  TEST_CHECK(activations == 2);

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
  TEST_CHECK(activations == 2);

  {
    ContextMenuPopup popup(wayland, renderContext);
    popup.setOnActivate([&activations](const ContextMenuControlEntry& entry) {
      TEST_CHECK(entry.id == 5);
      ++activations;
    });
    ContextMenuPopupTestAccess::activate(popup, ContextMenuControlEntry{.id = 4, .label = "Previous menu"});
    ContextMenuPopupTestAccess::dismiss(popup);
    popup.close();
    ContextMenuPopupTestAccess::activate(popup, ContextMenuControlEntry{.id = 5, .label = "Current menu"});
    drainDeferredCalls();
  }
  TEST_CHECK(activations == 3);

  return 0;
}
