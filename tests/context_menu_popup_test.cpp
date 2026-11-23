#include "core/deferred_call.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu_geometry.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"

#include <memory>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

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

  static void testInput(ContextMenuPopup& popup) {
    constexpr auto noHighlight = ContextMenuControl::NoHighlight;
    using Source = ContextMenuPopup::HighlightSource;
    const std::vector<ContextMenuControlEntry> entries{
        {.id = 1, .label = "Alpha", .submenuEntries = {{.id = 100, .label = "Leaf"}}},
        {.separator = true},
        {.id = 2, .label = "Bravo"},
        {.id = 3, .label = "Beta"},
        {.id = 4, .label = "Delta"},
        {.id = 5, .label = "Echo"},
        {.id = 6, .label = "Foxtrot"},
        {.id = 7, .label = "Golf"},
        {.id = 8, .label = "Hotel"},
        {.id = 9, .label = "Indigo"},
        {.id = 10, .label = "Juliet"},
        {.id = 11, .label = "Kilo"},
        {.id = 12, .label = "Disabled", .enabled = false},
        {.separator = true},
    };

    // Run the real popup event routing and row hit testing on an uninitialized surface.
    auto menu = std::make_unique<ContextMenuControl>();
    auto* control = menu.get();
    control->setMenuWidth(240.0F);
    control->setMaxVisible(entries.size());
    control->setEntries(entries);
    control->setHighlightedIndex(noHighlight);
    control->setOnPointerHighlight([&popup](std::size_t index) { popup.handlePointerHighlight(index); });

    std::vector<bool> submenuOpenedByKeyboard;
    const auto recordSubmenuOpen = [&](const ContextMenuControlEntry& entry, float) {
      if (!entry.submenuEntries.empty())
        submenuOpenedByKeyboard.push_back(popup.m_keyboardOpeningSubmenu);
    };
    control->setOnSubmenuOpen(recordSubmenuOpen);
    control->setOnRowHover(recordSubmenuOpen);
    ScaledRenderer renderer(popup.m_renderContext, 1.0F);
    control->layout(renderer);
    const float rowY = (control->rowTop(0) + control->rowBottom(0)) / 2.0F;

    int surfaceIdentity = 0;
    popup.m_wlSurface = reinterpret_cast<wl_surface*>(&surfaceIdentity);
    popup.m_surface = std::make_unique<PopupSurface>(popup.m_wayland);
    popup.m_menu = control;
    popup.m_sceneRoot = std::move(menu);
    popup.m_inputDispatcher.setSceneRoot(control);
    popup.m_highlightedIndex = noHighlight;
    const auto pointer = [&](PointerEvent::Type type, double x) {
      popup.onPointerEvent({.type = type, .surface = popup.m_wlSurface, .sx = x, .sy = rowY});
    };
    const auto key = [&](std::uint32_t sym, std::uint32_t utf32 = 0, bool repeat = false) {
      popup.onKeyboardEvent({.sym = sym, .utf32 = utf32, .pressed = true, .repeat = repeat});
    };
    const auto checkHighlight = [&](std::size_t index, Source source) {
      TEST_CHECK(control->highlightedIndex() == index);
      TEST_CHECK(popup.m_highlightedIndex == index);
      TEST_CHECK(popup.m_highlightSource == source);
    };

    pointer(PointerEvent::Type::Enter, 20.0);
    pointer(PointerEvent::Type::Motion, 20.5);
    checkHighlight(noHighlight, Source::None);
    TEST_CHECK(submenuOpenedByKeyboard.empty());
    pointer(PointerEvent::Type::Motion, 21.0);
    checkHighlight(0, Source::Pointer);
    TEST_CHECK(submenuOpenedByKeyboard == std::vector<bool>{false});

    popup.m_child = std::make_unique<ContextMenuPopup>(popup.m_wayland, popup.m_renderContext);
    popup.m_child->m_surface = std::make_unique<PopupSurface>(popup.m_wayland);
    pointer(PointerEvent::Type::Leave, 21.0);
    checkHighlight(0, Source::Pointer);
    popup.m_child.reset();
    pointer(PointerEvent::Type::Enter, 21.0);
    pointer(PointerEvent::Type::Leave, 21.0);
    checkHighlight(noHighlight, Source::None);

    control->setHighlightedIndex(2);
    popup.m_highlightedIndex = 2;
    popup.m_highlightSource = Source::Initial;
    pointer(PointerEvent::Type::Enter, 20.0);
    pointer(PointerEvent::Type::Leave, 20.0);
    checkHighlight(2, Source::Initial);

    pointer(PointerEvent::Type::Enter, 20.0);
    key(XKB_KEY_End);
    checkHighlight(11, Source::Keyboard);
    pointer(PointerEvent::Type::Motion, 20.75);
    checkHighlight(11, Source::Keyboard);
    pointer(PointerEvent::Type::Motion, 21.0);
    checkHighlight(0, Source::Pointer);
    key(XKB_KEY_End);
    pointer(PointerEvent::Type::Leave, 21.0);
    checkHighlight(11, Source::Keyboard);
    pointer(PointerEvent::Type::Enter, 20.0);
    checkHighlight(11, Source::Keyboard);

    key(XKB_KEY_Home);
    checkHighlight(0, Source::Keyboard);
    key(XKB_KEY_Page_Down);
    checkHighlight(9, Source::Keyboard);
    key(XKB_KEY_Page_Up);
    checkHighlight(0, Source::Keyboard);
    key(XKB_KEY_Right, 0, true);
    TEST_CHECK(submenuOpenedByKeyboard.size() == 2);
    key(XKB_KEY_Right);
    TEST_CHECK(submenuOpenedByKeyboard == (std::vector<bool>{false, false, true}));
    key(XKB_KEY_space, 0, true);
    TEST_CHECK(submenuOpenedByKeyboard.size() == 3);
    key(XKB_KEY_space);
    TEST_CHECK(submenuOpenedByKeyboard.size() == 4 && submenuOpenedByKeyboard.back());
    key(XKB_KEY_KP_Right);
    TEST_CHECK(submenuOpenedByKeyboard.size() == 5 && submenuOpenedByKeyboard.back());
    TEST_CHECK(!popup.m_keyboardOpeningSubmenu);

    key(XKB_KEY_b, 'b');
    checkHighlight(2, Source::Keyboard);
    key(XKB_KEY_b, 'b');
    checkHighlight(3, Source::Keyboard);
    key(XKB_KEY_r, 'r');
    checkHighlight(2, Source::Keyboard);
    popup.m_typeaheadLastTime -= std::chrono::seconds(1);
    key(XKB_KEY_e, 'e');
    checkHighlight(5, Source::Keyboard);

    std::vector<int> boundaryMoves;
    popup.setOnBoundaryHorizontalMove([&](int delta) { boundaryMoves.push_back(delta); });
    key(XKB_KEY_Left, 0, true);
    key(XKB_KEY_Left);
    key(XKB_KEY_Right);
    TEST_CHECK(boundaryMoves == (std::vector<int>{-1, 1}));

    popup.close();
    TEST_CHECK(!popup.isOpen() && !popup.m_pointerInside && !popup.m_pointerHoverArmed);
    TEST_CHECK(!popup.m_hasPointerAnchor && !popup.m_hasLastPointerPos);
    TEST_CHECK(popup.m_menu == nullptr && popup.m_highlightedIndex == noHighlight);
    TEST_CHECK(popup.m_highlightSource == Source::None && popup.m_typeaheadBuffer.empty());
  }
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

  {
    ContextMenuPopup popup(wayland, renderContext);
    ContextMenuPopupTestAccess::testInput(popup);
  }

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
