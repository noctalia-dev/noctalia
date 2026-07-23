#include "render/backend/render_backend.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/drag_source.h"
#include "ui/controls/drop_zone.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/spacer.h"
#include "ui/controls/toggle.h"
#include "ui/drag_drop_controller.h"
#include "ui/style.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <cstdlib>
#include <print>
#include <string>
#include <utility>
#include <vector>

// Satisfies the AsyncTextureCache link dependency pulled in by the Image
// control; never invoked — this test exercises no GPU path.
std::unique_ptr<TextureManager> createDefaultTextureManager() { return nullptr; }

namespace {

  // Reconciliation itself needs no real rendering; the renderer is only touched
  // by image/graph nodes, which this test does not exercise.
  class StubRenderer : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize, bool
    ) override {
      return TextMetrics{.width = static_cast<float>(text.size()) * fontSize * 0.5f, .bottom = fontSize};
    }
    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float fontSize, const std::vector<std::size_t>& byteOffsets, float,
        std::vector<TextCursorStop>& outStops, FontWeight
    ) override {
      outStops.assign(byteOffsets.size(), TextCursorStop{0.0f, 0.0f, fontSize});
    }
    TextMetrics measureGlyph(char32_t, float fontSize) override {
      return TextMetrics{.width = fontSize, .bottom = fontSize};
    }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0f; }
  };

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "ui_tree_reconciler_test: {}", message);
      return false;
    }
    return true;
  }

  ui::UiTreeNode makeNode(std::string type) {
    ui::UiTreeNode node;
    node.type = std::move(type);
    return node;
  }

  ui::UiTreeNode makeLabel(std::string text, std::string key = {}) {
    ui::UiTreeNode node = makeNode("label");
    node.key = std::move(key);
    node.props.emplace("text", std::move(text));
    return node;
  }

  ui::UiTreeNode
  makeDragSource(std::string key = "source", std::string dragType = "keybind", std::string payload = "bind:1") {
    ui::UiTreeNode node = makeNode("drag_source");
    node.key = std::move(key);
    node.props.emplace("dragType", std::move(dragType));
    node.props.emplace("payload", std::move(payload));
    node.props.emplace("width", 30.0);
    node.props.emplace("height", 30.0);
    return node;
  }

  ui::UiTreeNode makeDropZone(
      std::string key = "zone", std::vector<std::string> accepts = {"keybind"}, std::string value = "category:2",
      std::string onDrop = "onKeybindDropped"
  ) {
    ui::UiTreeNode node = makeNode("drop_zone");
    node.key = std::move(key);
    node.props.emplace("accepts", std::move(accepts));
    node.props.emplace("value", std::move(value));
    node.props.emplace("onDrop", std::move(onDrop));
    node.props.emplace("width", 70.0);
    node.props.emplace("height", 40.0);
    return node;
  }

  void layoutDragTree(Flex& host, DragSource* source, StubRenderer& renderer) {
    host.setSize(400.0f, 120.0f);
    host.layout(renderer);
    (void)source;
  }

  void sourceLocalPointAt(
      const DragSource& source, const Node& target, float targetX, float targetY, float& localX, float& localY
  ) {
    float sceneX = 0.0f;
    float sceneY = 0.0f;
    Node::mapToScene(&target, targetX, targetY, sceneX, sceneY);
    (void)Node::mapFromScene(source.inputArea(), sceneX, sceneY, localX, localY);
  }

  void sourceLocalPointFor(const DragSource& source, const Node& target, float& localX, float& localY) {
    sourceLocalPointAt(source, target, target.width() * 0.5f, target.height() * 0.5f, localX, localY);
  }

} // namespace

int main() {
  bool ok = true;
  StubRenderer renderer;

  // Build: column{ label, box, spacer } reconciled as the host's single child.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    tree.props.emplace("gap", 8.0);
    tree.children.push_back(makeLabel("Hello"));
    ui::UiTreeNode box = makeNode("box");
    box.props.emplace("width", 10.0);
    box.props.emplace("height", 4.0);
    tree.children.push_back(box);
    tree.children.push_back(makeNode("spacer"));

    ok = expect(reconciler.reconcile(host, tree, renderer), "initial reconcile reports structure change") && ok;
    ok = expect(host.children().size() == 1, "host has one root child") && ok;
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(column != nullptr, "root child is a Flex") && ok;
    if (column != nullptr) {
      ok = expect(column->gap() == 8.0f, "gap applied") && ok;
      ok = expect(column->align() == FlexAlign::Stretch, "ui column defaults to stretch") && ok;
      ok = expect(column->children().size() == 3, "column has three children") && ok;
      auto* label = dynamic_cast<Label*>(column->children()[0].get());
      ok = expect(label != nullptr && label->text() == "Hello", "label text applied") && ok;
      ok = expect(dynamic_cast<Box*>(column->children()[1].get()) != nullptr, "second child is a Box") && ok;
      ok = expect(dynamic_cast<Spacer*>(column->children()[2].get()) != nullptr, "third child is a Spacer") && ok;
    }

    // In-place update: same structure, changed props -> same control instances.
    Node* labelBefore = column != nullptr ? column->children()[0].get() : nullptr;
    tree.props["gap"] = 4.0;
    tree.children[0].props["text"] = std::string("World");
    ok = expect(!reconciler.reconcile(host, tree, renderer), "prop-only reconcile reports no structure change") && ok;
    if (column != nullptr) {
      ok = expect(column->gap() == 4.0f, "gap updated in place") && ok;
      ok = expect(column->children()[0].get() == labelBefore, "label instance reused") && ok;
      auto* label = dynamic_cast<Label*>(column->children()[0].get());
      ok = expect(label != nullptr && label->text() == "World", "label text updated") && ok;
    }

    // Removal: drop to a single child.
    tree.children.resize(1);
    ok = expect(reconciler.reconcile(host, tree, renderer), "removal reports structure change") && ok;
    if (column != nullptr) {
      ok = expect(column->children().size() == 1, "children removed") && ok;
      ok = expect(column->children()[0].get() == labelBefore, "surviving child reused on removal") && ok;
    }
  }

  // Keyed reorder reuses control instances.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("row");
    tree.children.push_back(makeLabel("A", "a"));
    tree.children.push_back(makeLabel("B", "b"));
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(row != nullptr && row->children().size() == 2, "keyed row built") && ok;
    Node* first = row != nullptr ? row->children()[0].get() : nullptr;
    Node* second = row != nullptr ? row->children()[1].get() : nullptr;

    std::swap(tree.children[0], tree.children[1]);
    ok = expect(reconciler.reconcile(host, tree, renderer), "reorder reports structure change") && ok;
    if (row != nullptr) {
      ok = expect(
               row->children()[0].get() == second && row->children()[1].get() == first,
               "keyed children reuse instances across reorder"
           )
          && ok;
    }
  }

  // Type change replaces the control; unknown types are skipped without crashing.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    tree.children.push_back(makeLabel("X"));
    (void)reconciler.reconcile(host, tree, renderer);
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    Node* labelNode = column != nullptr ? column->children()[0].get() : nullptr;

    tree.children[0] = makeNode("box");
    ok = expect(reconciler.reconcile(host, tree, renderer), "type change reports structure change") && ok;
    if (column != nullptr) {
      ok = expect(column->children().size() == 1, "type change keeps single child") && ok;
      ok = expect(column->children()[0].get() != labelNode, "type change replaces the instance") && ok;
      ok = expect(dynamic_cast<Box*>(column->children()[0].get()) != nullptr, "replacement is a Box") && ok;
    }

    tree.children.clear();
    tree.children.push_back(makeNode("definitely-not-a-control"));
    tree.children.push_back(makeLabel("Y"));
    (void)reconciler.reconcile(host, tree, renderer);
    if (column != nullptr) {
      ok = expect(column->children().size() == 1, "unknown type skipped") && ok;
      auto* label = dynamic_cast<Label*>(column->children()[0].get());
      ok = expect(label != nullptr && label->text() == "Y", "valid sibling of unknown type still built") && ok;
    }
  }

  // Scale multiplies size-like props.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setScale(2.0f);
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode label = makeLabel("S");
    label.props.emplace("fontSize", 10.0);
    tree.children.push_back(label);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* scaled = column != nullptr ? dynamic_cast<Label*>(column->children()[0].get()) : nullptr;
    ok = expect(scaled != nullptr && scaled->fontSize() == 20.0f, "fontSize scaled by content scale") && ok;
  }

  // Button onClick routes through the callback sink.
  {
    ui::UiTreeReconciler reconciler;
    std::string fired;
    reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) { fired = cb.fn; });
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode button = makeNode("button");
    button.props.emplace("text", std::string("Go"));
    button.props.emplace("onClick", std::string("openDetails"));
    tree.children.push_back(button);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* control = column != nullptr ? dynamic_cast<Button*>(column->children()[0].get()) : nullptr;
    ok = expect(control != nullptr, "button built") && ok;
    // The sink wiring is exercised via the reconciler-installed callback.
    ok = expect(fired.empty(), "sink not fired before click") && ok;
  }

  // The global button-border style updates existing buttons and preserves custom widths.
  {
    Style::setButtonBordersEnabled(true);
    Button button;
    auto backgroundBorderWidth = [&button]() {
      for (const auto& child : button.children()) {
        if (const auto* background = dynamic_cast<const RectNode*>(child.get())) {
          return background->style().borderWidth;
        }
      }
      return -1.0f;
    };

    ok = expect(backgroundBorderWidth() == Style::borderWidth, "button border enabled by default") && ok;

    Button::ButtonPalette custom = Button::defaultPalette(ButtonVariant::Default);
    custom.borderWidth = 4.0f;
    button.setCustomPalette(custom);
    ok = expect(backgroundBorderWidth() == 4.0f, "custom button border width applied") && ok;

    Style::setButtonBordersEnabled(false);
    ok = expect(backgroundBorderWidth() == 0.0f, "button border removed by global style") && ok;

    Style::setButtonBordersEnabled(true);
    ok = expect(backgroundBorderWidth() == 4.0f, "custom button border width restored") && ok;
  }

  // Toggling a box's onClick across reconciles rebuilds it: a clickable box is
  // wrapped in an InputArea (so it is not directly a Box), a bare box is not.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    tree.children.push_back(makeNode("box"));
    (void)reconciler.reconcile(host, tree, renderer);
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    Node* bareBox = column != nullptr ? column->children()[0].get() : nullptr;
    ok = expect(dynamic_cast<Box*>(bareBox) != nullptr, "bare box is an unwrapped Box") && ok;

    tree.children[0].props.emplace("onClick", std::string("openDetails"));
    (void)reconciler.reconcile(host, tree, renderer);
    Node* clickableBox = column != nullptr ? column->children()[0].get() : nullptr;
    ok = expect(clickableBox != bareBox, "adding onClick rebuilds the box") && ok;
    ok = expect(dynamic_cast<Box*>(clickableBox) == nullptr, "clickable box is wrapped, not a bare Box") && ok;

    tree.children[0].props.erase("onClick");
    (void)reconciler.reconcile(host, tree, renderer);
    Node* unwrapped = column != nullptr ? column->children()[0].get() : nullptr;
    ok = expect(unwrapped != clickableBox, "removing onClick rebuilds the box") && ok;
    ok = expect(dynamic_cast<Box*>(unwrapped) != nullptr, "box unwrapped after onClick removed") && ok;
  }

  // Clickable row and column containers preserve the Flex as the wrapped child,
  // forward its content measurement, and route pointer callbacks.
  for (const char* type : {"row", "column"}) {
    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onClick", std::string("activate"));
      tree.children.push_back(makeLabel("First"));
      tree.children.push_back(makeLabel("Second"));
      (void)reconciler.reconcile(host, tree, renderer);

      Node* root = host.children().empty() ? nullptr : host.children().front().get();
      auto* area = dynamic_cast<InputArea*>(root);
      auto* inner =
          area != nullptr && !area->children().empty() ? dynamic_cast<Flex*>(area->children().front().get()) : nullptr;
      ok = expect(dynamic_cast<Flex*>(root) == nullptr, "clickable container is not a bare Flex") && ok;
      ok = expect(area != nullptr, "clickable container is an InputArea") && ok;
      ok = expect(inner != nullptr && inner->children().size() == 2, "labels reconcile into inner Flex") && ok;

      LayoutSize innerSize{};
      LayoutSize wrappedSize{};
      if (area != nullptr && inner != nullptr) {
        innerSize = inner->measure(renderer, LayoutConstraints::unconstrained());
        wrappedSize = area->measure(renderer, LayoutConstraints::unconstrained());
      }
      ok = expect(
               innerSize.width == wrappedSize.width
                   && innerSize.height == wrappedSize.height
                   && innerSize.width > 0.0f
                   && innerSize.height > 0.0f,
               "clickable container forwards content measurement"
           )
          && ok;
    }

    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onClick", std::string("activate"));
      tree.props.emplace("width", 120.0);
      tree.props.emplace("height", 36.0);
      tree.children.push_back(makeLabel("Sized"));
      (void)reconciler.reconcile(host, tree, renderer);

      auto* area = dynamic_cast<InputArea*>(host.children().front().get());
      auto* inner =
          area != nullptr && !area->children().empty() ? dynamic_cast<Flex*>(area->children().front().get()) : nullptr;
      LayoutSize measured{};
      if (area != nullptr) {
        measured = area->measure(renderer, LayoutConstraints::unconstrained());
      }
      ok = expect(
               inner != nullptr
                   && inner->minWidth() == 120.0f
                   && inner->maxWidth() == 120.0f
                   && inner->minHeight() == 36.0f
                   && inner->maxHeight() == 36.0f
                   && measured.width == 120.0f
                   && measured.height == 36.0f,
               "clickable container preserves explicit size"
           )
          && ok;
    }

    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onClick", std::string("activate"));
      (void)reconciler.reconcile(host, tree, renderer);
      auto* clickable = dynamic_cast<InputArea*>(host.children().front().get());
      ok = expect(
               clickable != nullptr
                   && clickable->acceptedButtons() == InputArea::buttonMask(BTN_LEFT)
                   && clickable->focusable(),
               "clickable container accepts left clicks and is focusable"
           )
          && ok;

      // Validate matching uses the application's global keybind matcher, which this test does not configure.
    }

    // Hover-only wrappers start with an empty button mask and never take
    // focus. (The retained-node route to the same state — onClick removed
    // while onHover stays — is covered by the clearing test above.)
    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onHover", std::string("hover"));
      (void)reconciler.reconcile(host, tree, renderer);
      auto* hoverOnly = dynamic_cast<InputArea*>(host.children().front().get());
      ok = expect(
               hoverOnly != nullptr && hoverOnly->acceptedButtons() == 0 && !hoverOnly->focusable(),
               "hover-only container does not accept clicks or take focus"
           )
          && ok;
    }

    {
      ui::UiTreeReconciler reconciler;
      std::string fired;
      reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) { fired = cb.fn; });
      Flex host;

      // Explicit size: dispatchPress hit-tests the point against the wrapper's
      // bounds, and an empty content-sized container measures 0x0.
      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onClick", std::string("first"));
      tree.props.emplace("width", 40.0);
      tree.props.emplace("height", 20.0);
      (void)reconciler.reconcile(host, tree, renderer);
      auto* area = dynamic_cast<InputArea*>(host.children().front().get());
      if (area != nullptr) {
        (void)area->measure(renderer, LayoutConstraints::unconstrained());
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, true);
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, false);
      }
      ok = expect(fired == "first", "click dispatch reaches the first container callback") && ok;

      tree.props["onClick"] = std::string("second");
      (void)reconciler.reconcile(host, tree, renderer);
      fired.clear();
      if (area != nullptr) {
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, true);
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, false);
      }
      ok = expect(fired == "second", "reconciled container callback replaces the old click callback") && ok;

      // onClick emptied while onHover keeps the wrapper alive: wrapper input
      // handlers clear on removal (deviating from the retain-absent-props
      // default) — a retained one would leave an invisible click-swallowing,
      // keyboard-activatable node. The wrapper drops mask, focus, and handler.
      tree.props["onClick"] = std::string();
      tree.props.emplace("onHover", std::string("h"));
      (void)reconciler.reconcile(host, tree, renderer);
      fired.clear();
      if (area != nullptr) {
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, true);
        area->dispatchPress(5.0f, 5.0f, BTN_LEFT, false);
      }
      ok = expect(
               fired.empty()
                   && area != nullptr
                   && area->acceptedButtons() == 0
                   && !area->focusable()
                   && area->cursorShape() == 0,
               "emptied onClick clears the wrapper's activation"
           )
          && ok;
    }

    // An empty callback name is "unset": wiring is change-detected against the
    // slot's empty default, so it could never fire — wrapping anyway would
    // swallow ancestor clicks/hover and add a dead tab stop.
    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      tree.props.emplace("onClick", std::string());
      tree.props.emplace("onHover", std::string());
      (void)reconciler.reconcile(host, tree, renderer);
      Node* node = host.children().empty() ? nullptr : host.children().front().get();
      ok = expect(dynamic_cast<Flex*>(node) != nullptr, "empty callback names leave the container unwrapped") && ok;
    }

    {
      ui::UiTreeReconciler reconciler;
      std::string callbackName;
      std::string callbackArg;
      std::string callbackKey;
      reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback& cb) {
        callbackName = cb.fn;
        callbackArg = cb.arg1;
        callbackKey = cb.arg2;
      });
      Flex host;

      // The node's key rides along as arg2, so one handler can serve a list.
      ui::UiTreeNode tree = makeNode(type);
      tree.key = "chip-3";
      tree.props.emplace("onHover", std::string("hover"));
      (void)reconciler.reconcile(host, tree, renderer);
      auto* area = dynamic_cast<InputArea*>(host.children().front().get());
      if (area != nullptr) {
        area->dispatchEnter(0.0f, 0.0f);
        ok = expect(
                 callbackName == "hover" && callbackArg == "true" && callbackKey == "chip-3",
                 "container hover enter reaches the sink with the node key"
             )
            && ok;
        area->dispatchLeave();
      }
      ok = expect(
               callbackName == "hover" && callbackArg == "false" && callbackKey == "chip-3",
               "container hover leave reaches the sink with the node key"
           )
          && ok;
    }

    {
      ui::UiTreeReconciler reconciler;
      Flex host;

      ui::UiTreeNode tree = makeNode(type);
      (void)reconciler.reconcile(host, tree, renderer);
      Node* bare = host.children().empty() ? nullptr : host.children().front().get();
      ok = expect(dynamic_cast<Flex*>(bare) != nullptr, "bare row/column remains a plain Flex") && ok;

      tree.props.emplace("onClick", std::string("activate"));
      (void)reconciler.reconcile(host, tree, renderer);
      Node* wrapped = host.children().empty() ? nullptr : host.children().front().get();
      ok =
          expect(wrapped != bare && dynamic_cast<InputArea*>(wrapped) != nullptr, "adding onClick rebuilds flex") && ok;

      tree.props.erase("onClick");
      (void)reconciler.reconcile(host, tree, renderer);
      Node* unwrapped = host.children().empty() ? nullptr : host.children().front().get();
      ok = expect(
               unwrapped != wrapped && dynamic_cast<Flex*>(unwrapped) != nullptr,
               "removing onClick rebuilds flex without wrapper"
           )
          && ok;
    }
  }

  // Hover state a plugin mirrors must stay balanced: a hovered node dropped by a
  // reconcile is destroyed without the dispatcher ever sending leave, so the
  // reconciler emits the closing "false" itself.
  {
    ui::UiTreeReconciler reconciler;
    std::vector<std::pair<std::string, std::string>> fired;
    reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) {
      fired.emplace_back(cb.fn, cb.arg1);
    });
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode chip = makeNode("box");
    chip.key = "a";
    chip.props.emplace("onHover", std::string("chipHover"));
    tree.children.push_back(chip);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* chipArea = column != nullptr && !column->children().empty()
        ? dynamic_cast<InputArea*>(column->children()[0].get())
        : nullptr;
    ok = expect(chipArea != nullptr, "hovered-drop fixture built a wrapped box") && ok;
    if (chipArea != nullptr) {
      chipArea->dispatchEnter(0.0f, 0.0f);
    }
    ok = expect(fired.size() == 1 && fired.front().second == "true", "hover enter reported before the drop") && ok;

    // A retained hovered node is not a drop, so nothing more fires.
    fired.clear();
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(fired.empty(), "retaining a hovered node emits no hover callback") && ok;

    tree.children.clear();
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(
             fired.size() == 1 && fired.front().first == "chipHover" && fired.front().second == "false",
             "dropping a hovered node emits the closing onHover false"
         )
        && ok;
  }

  // The same balance holds for the other two ways a hover ends without the
  // dispatcher: the callback is rewired or dropped, and the whole host tree is
  // torn down and reset out from under the reconciler.
  {
    ui::UiTreeReconciler reconciler;
    std::vector<std::pair<std::string, std::string>> fired;
    reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) {
      fired.emplace_back(cb.fn, cb.arg1);
    });
    Flex host;

    ui::UiTreeNode tree = makeNode("box");
    tree.props.emplace("onHover", std::string("first"));
    (void)reconciler.reconcile(host, tree, renderer);
    auto* area = dynamic_cast<InputArea*>(host.children().front().get());
    ok = expect(area != nullptr, "rewire fixture built a wrapped box") && ok;
    if (area != nullptr) {
      area->dispatchEnter(0.0f, 0.0f);
    }
    fired.clear();

    // Rewiring while hovered closes the old callback; the new one opens on the
    // next enter, which the dispatcher will deliver against the retained node.
    tree.props["onHover"] = std::string("second");
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(
             fired.size() == 1 && fired.front().first == "first" && fired.front().second == "false",
             "rewiring onHover while hovered closes the old callback"
         )
        && ok;

    fired.clear();
    if (area != nullptr) {
      area->dispatchEnter(0.0f, 0.0f);
    }
    ok = expect(fired.size() == 1 && fired.front().first == "second", "the rewired callback opens on re-enter") && ok;

    // A host tree torn down and reset away leaves slots naming a hover whose
    // Nodes are already freed, so reset() must still close it.
    fired.clear();
    reconciler.reset();
    ok = expect(
             fired.size() == 1 && fired.front().first == "second" && fired.front().second == "false",
             "reset closes a hover left open by a torn-down tree"
         )
        && ok;

    fired.clear();
    reconciler.reset();
    ok = expect(fired.empty(), "a second reset has no hover left to close") && ok;
  }

  // Siblings sharing one onHover handler are told apart by node, not by callback
  // name: dropping the unhovered sibling must not close the hovered one.
  {
    ui::UiTreeReconciler reconciler;
    std::vector<std::pair<std::string, std::string>> fired;
    reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) {
      fired.emplace_back(cb.arg1, cb.arg2);
    });
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    for (const char* key : {"a", "b"}) {
      ui::UiTreeNode chip = makeNode("box");
      chip.key = key;
      chip.props.emplace("onHover", std::string("chipHover"));
      tree.children.push_back(chip);
    }
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* first = column != nullptr && column->children().size() == 2
        ? dynamic_cast<InputArea*>(column->children()[0].get())
        : nullptr;
    ok = expect(first != nullptr, "shared-name fixture built two wrapped boxes") && ok;
    if (first != nullptr) {
      first->dispatchEnter(0.0f, 0.0f);
    }
    ok = expect(fired.size() == 1 && fired.front().second == "a", "hover opens against the keyed node") && ok;

    // Drop the sibling that is not hovered.
    fired.clear();
    tree.children.pop_back();
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(fired.empty(), "dropping an unhovered same-name sibling leaves the hover open") && ok;

    // Now drop the hovered one; it closes with its own key.
    tree.children.clear();
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(
             fired.size() == 1 && fired.front().first == "false" && fired.front().second == "a",
             "dropping the hovered node closes it with its own key"
         )
        && ok;
  }

  // Button hover callbacks clear on removal rather than being retained, and an
  // empty callback name counts as unset.
  {
    ui::UiTreeReconciler reconciler;
    std::string fired;
    reconciler.setCallbackSink([&fired](const ui::UiTreeReconciler::ControlCallback& cb) {
      fired = cb.fn + "/" + cb.arg1;
    });
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode button = makeNode("button");
    button.props.emplace("text", std::string("Go"));
    button.props.emplace("onHover", std::string("hover"));
    tree.children.push_back(button);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* control = column != nullptr ? dynamic_cast<Button*>(column->children()[0].get()) : nullptr;
    auto* area = control != nullptr ? control->inputArea() : nullptr;
    ok = expect(area != nullptr, "hover button built") && ok;
    if (area != nullptr) {
      area->dispatchEnter(0.0f, 0.0f);
      ok = expect(fired == "hover/true", "button onHover enter reaches the sink") && ok;
      area->dispatchLeave();
      ok = expect(fired == "hover/false", "button onHover leave reaches the sink") && ok;

      tree.children[0].props.erase("onHover");
      (void)reconciler.reconcile(host, tree, renderer);
      fired.clear();
      area->dispatchEnter(0.0f, 0.0f);
      area->dispatchLeave();
      ok = expect(fired.empty(), "dropped button onHover stops firing") && ok;

      tree.children[0].props.emplace("onHover", std::string());
      (void)reconciler.reconcile(host, tree, renderer);
      area->dispatchEnter(0.0f, 0.0f);
      area->dispatchLeave();
      ok = expect(fired.empty(), "an empty button onHover name wires nothing") && ok;
    }
  }

  // Interactive controls build and apply their value props.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");

    ui::UiTreeNode toggle = makeNode("toggle");
    toggle.props.emplace("checked", true);
    tree.children.push_back(toggle);

    ui::UiTreeNode slider = makeNode("slider");
    slider.props.emplace("min", 0.0);
    slider.props.emplace("max", 10.0);
    slider.props.emplace("value", 3.0);
    tree.children.push_back(slider);

    ui::UiTreeNode select = makeNode("select");
    select.props.emplace("options", std::vector<std::string>{"red", "green", "blue"});
    select.props.emplace("selectedIndex", 2.0);
    tree.children.push_back(select);

    (void)reconciler.reconcile(host, tree, renderer);
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(column != nullptr && column->children().size() == 3, "interactive column built") && ok;
    if (column != nullptr) {
      auto* tog = dynamic_cast<Toggle*>(column->children()[0].get());
      ok = expect(tog != nullptr && tog->checked(), "toggle checked applied") && ok;
      auto* sld = dynamic_cast<Slider*>(column->children()[1].get());
      ok = expect(sld != nullptr && sld->value() == 3.0 && sld->maxValue() == 10.0, "slider range+value applied") && ok;
      auto* sel = dynamic_cast<Select*>(column->children()[2].get());
      ok = expect(
               sel != nullptr && sel->selectedIndex() == 2 && sel->selectedText() == "blue",
               "select options+index applied"
           )
          && ok;
    }
  }

  // Select callbacks report the host's 0-based option index and selected text.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;
    std::string callbackName;
    std::string callbackIndex;
    std::string callbackText;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback& cb) {
      callbackName = cb.fn;
      callbackIndex = cb.arg1;
      callbackText = cb.arg2;
    });

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode select = makeNode("select");
    select.key = "target";
    select.props.emplace("options", std::vector<std::string>{"All outputs", "DP-1", "DP-2"});
    select.props.emplace("selectedIndex", 0.0);
    select.props.emplace("onChange", std::string("onOutputChange"));
    tree.children.push_back(select);

    (void)reconciler.reconcile(host, tree, renderer);
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* sel = column != nullptr ? dynamic_cast<Select*>(column->children()[0].get()) : nullptr;
    ok = expect(sel != nullptr, "select callback control built") && ok;
    if (sel != nullptr) {
      sel->setSelectedIndex(1);
      ok = expect(callbackName == "onOutputChange", "select callback name fired") && ok;
      ok = expect(callbackIndex == "1", "select callback reports selected index") && ok;
      ok = expect(callbackText == "DP-1", "select callback reports selected text") && ok;

      callbackName.clear();
      callbackIndex.clear();
      callbackText.clear();
      tree.children[0].props["selectedIndex"] = 0.0;
      (void)reconciler.reconcile(host, tree, renderer);
      ok = expect(
               sel->selectedIndex() == 0 && sel->selectedText() == "All outputs", "select controlled index re-applied"
           )
          && ok;
      ok = expect(
               callbackName.empty() && callbackIndex.empty() && callbackText.empty(),
               "select controlled sync does not fire callback"
           )
          && ok;
    }
  }

  // Input is uncontrolled: `value` seeds once and later renders never overwrite it.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode input = makeNode("input");
    input.key = "field";
    input.props.emplace("value", std::string("seed"));
    tree.children.push_back(input);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* in = column != nullptr ? dynamic_cast<Input*>(column->children()[0].get()) : nullptr;
    ok = expect(in != nullptr && in->value() == "seed", "input seeded with initial value") && ok;
    Node* inputBefore = in;

    // Re-render with a different declared value — the host-owned buffer is kept.
    tree.children[0].props["value"] = std::string("changed");
    (void)reconciler.reconcile(host, tree, renderer);
    if (column != nullptr) {
      ok = expect(column->children()[0].get() == inputBefore, "keyed input instance reused") && ok;
      ok = expect(in != nullptr && in->value() == "seed", "uncontrolled input not overwritten on re-render") && ok;
    }
  }

  // Multiline input: the prop applies, seeds a multi-line value, and the keyed
  // instance survives re-renders like any other input.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode input = makeNode("input");
    input.key = "editor";
    input.props.emplace("value", std::string("line one\nline two"));
    input.props.emplace("multiline", true);
    tree.children.push_back(input);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* in = column != nullptr ? dynamic_cast<Input*>(column->children()[0].get()) : nullptr;
    ok =
        expect(in != nullptr && in->value() == "line one\nline two", "multiline input seeded with newline value") && ok;
    Node* inputBefore = in;

    (void)reconciler.reconcile(host, tree, renderer);
    if (column != nullptr) {
      ok = expect(column->children()[0].get() == inputBefore, "keyed multiline input instance reused") && ok;
    }
  }

  // reset() discards retained slots so a reconciler survives its host tree being
  // destroyed and rebuilt (a panel reopened after close), with no use-after-free.
  {
    ui::UiTreeReconciler reconciler;
    ui::UiTreeNode tree = makeNode("column");
    tree.children.push_back(makeLabel("A"));
    {
      Flex host1;
      (void)reconciler.reconcile(host1, tree, renderer);
    } // host1 (and the controls the reconciler created in it) destroyed here
    reconciler.reset();
    Flex host2;
    (void)reconciler.reconcile(host2, tree, renderer);
    ok = expect(host2.children().size() == 1, "reset() lets the reconciler rebuild into a fresh host") && ok;
    auto* column = dynamic_cast<Flex*>(host2.children().front().get());
    ok = expect(column != nullptr && column->children().size() == 1, "rebuilt tree has its child") && ok;
  }

  // ScrollView hosts declared children in its inner content Flex.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode scroll = makeNode("scroll");
    scroll.children.push_back(makeLabel("one"));
    scroll.children.push_back(makeLabel("two"));
    tree.children.push_back(scroll);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* sv = column != nullptr ? dynamic_cast<ScrollView*>(column->children()[0].get()) : nullptr;
    ok = expect(sv != nullptr, "scroll built") && ok;
    if (sv != nullptr) {
      ok = expect(sv->content()->children().size() == 2, "scroll children land in content flex") && ok;
      auto* label = dynamic_cast<Label*>(sv->content()->children()[0].get());
      ok = expect(label != nullptr && label->text() == "one", "scroll child label applied") && ok;
    }
  }

  // A button tooltip reaches the InputArea, and dropping the prop clears it on
  // the retained control.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode button = makeNode("button");
    button.props.emplace("text", std::string("Go"));
    button.props.emplace("tooltip", std::string("Run the thing"));
    tree.children.push_back(button);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* control = column != nullptr ? dynamic_cast<Button*>(column->children()[0].get()) : nullptr;
    ok = expect(control != nullptr && control->inputArea() != nullptr, "tooltip button built") && ok;
    if (control != nullptr && control->inputArea() != nullptr) {
      ok = expect(control->inputArea()->hasTooltip(), "button tooltip applied") && ok;

      tree.children[0].props.erase("tooltip");
      (void)reconciler.reconcile(host, tree, renderer);
      ok = expect(!control->inputArea()->hasTooltip(), "dropped tooltip prop clears the tooltip") && ok;
    }
  }

  // The `size` tier pins the control height, scaled by the content scale.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setScale(2.0f);
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode button = makeNode("button");
    button.props.emplace("text", std::string("Go"));
    button.props.emplace("controlSize", std::string("sm"));
    tree.children.push_back(button);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    auto* control = column != nullptr ? dynamic_cast<Button*>(column->children()[0].get()) : nullptr;
    const float expected = Style::controlHeightSm * 2.0f;
    ok = expect(
             control != nullptr && control->minHeight() == expected && control->maxHeight() == expected,
             "button controlSize 'sm' pins the scaled small control height"
         )
        && ok;
  }

  // The controlSize tier drives input, select and slider through setControlHeight().
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    for (const char* type : {"input", "select", "slider"}) {
      ui::UiTreeNode control = makeNode(type);
      control.props.emplace("controlSize", std::string("sm"));
      tree.children.push_back(control);
    }
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(column != nullptr && column->children().size() == 3, "sized controls built") && ok;
    if (column != nullptr && column->children().size() == 3) {
      for (const auto& child : column->children()) {
        const LayoutSize size = child->measure(renderer, LayoutConstraints::unconstrained());
        ok = expect(size.height == Style::controlHeightSm, "controlSize 'sm' measures at the small control height")
            && ok;
      }
    }
  }

  // An explicit numeric height wins over the controlSize tier; an unknown tier is a
  // warn-and-ignore that leaves the default height.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode pinned = makeNode("button");
    pinned.key = "pinned";
    pinned.props.emplace("text", std::string("Go"));
    pinned.props.emplace("controlSize", std::string("sm"));
    pinned.props.emplace("height", 50.0);
    tree.children.push_back(pinned);

    ui::UiTreeNode bogus = makeNode("button");
    bogus.key = "bogus";
    bogus.props.emplace("text", std::string("Go"));
    bogus.props.emplace("controlSize", std::string("tiny"));
    tree.children.push_back(bogus);

    // A numeric controlSize is a warn-and-ignore, not a silent no-op: the tier
    // is a string, and `height` is the prop for exact pixels.
    ui::UiTreeNode numeric = makeNode("button");
    numeric.key = "numeric";
    numeric.props.emplace("text", std::string("Go"));
    numeric.props.emplace("controlSize", 32.0);
    tree.children.push_back(numeric);

    ui::UiTreeNode plain = makeNode("button");
    plain.key = "plain";
    plain.props.emplace("text", std::string("Go"));
    tree.children.push_back(plain);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(column != nullptr && column->children().size() == 4, "height-vs-controlSize buttons built") && ok;
    if (column != nullptr && column->children().size() == 4) {
      auto* pinnedButton = dynamic_cast<Button*>(column->children()[0].get());
      auto* bogusButton = dynamic_cast<Button*>(column->children()[1].get());
      auto* numericButton = dynamic_cast<Button*>(column->children()[2].get());
      auto* plainButton = dynamic_cast<Button*>(column->children()[3].get());
      ok = expect(
               pinnedButton != nullptr && pinnedButton->minHeight() == 50.0f && pinnedButton->maxHeight() == 50.0f,
               "explicit height wins over the controlSize tier"
           )
          && ok;
      ok = expect(
               bogusButton != nullptr && plainButton != nullptr && bogusButton->minHeight() == plainButton->minHeight(),
               "unknown controlSize tier leaves the default height"
           )
          && ok;
      ok = expect(
               numericButton != nullptr
                   && plainButton != nullptr
                   && numericButton->minHeight() == plainButton->minHeight(),
               "numeric controlSize is ignored, leaving the default height"
           )
          && ok;
    }
  }

  // Drag-and-drop controls are panel-only. With the capability disabled both
  // wrappers (and their subtrees) are skipped; enabling it builds the same tree.
  {
    ui::UiTreeReconciler reconciler;
    Flex host;

    ui::UiTreeNode tree = makeNode("column");
    ui::UiTreeNode source = makeDragSource();
    source.children.push_back(makeLabel("grip"));
    ui::UiTreeNode zone = makeDropZone();
    zone.children.push_back(makeLabel("target"));
    tree.children.push_back(std::move(source));
    tree.children.push_back(std::move(zone));

    (void)reconciler.reconcile(host, tree, renderer);
    auto* column = dynamic_cast<Flex*>(host.children().front().get());
    ok = expect(column != nullptr, "DnD gating keeps the supported parent") && ok;
    ok = expect(column != nullptr && column->children().empty(), "DnD controls skipped outside plugin panels") && ok;

    reconciler.setDragDropEnabled(true);
    (void)reconciler.reconcile(host, tree, renderer);
    ok = expect(column != nullptr && column->children().size() == 2, "DnD controls build when panel capability is on")
        && ok;
    if (column != nullptr && column->children().size() == 2) {
      ok = expect(dynamic_cast<DragSource*>(column->children()[0].get()) != nullptr, "panel builds DragSource") && ok;
      ok = expect(dynamic_cast<DropZone*>(column->children()[1].get()) != nullptr, "panel builds DropZone") && ok;
    }
  }

  // Required DnD props are strict on every reconciliation. Invalid props disable
  // and clear a retained keyed control rather than preserving its previous data.
  // An explicitly empty string-array accepts list remains valid and accepts nothing.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;

    ui::UiTreeNode validSource = makeDragSource("source", "keybind", "bind:original");
    validSource.props.emplace("tooltip", std::string("Drag keybind"));
    ui::UiTreeNode validZone = makeDropZone("zone", {"keybind", "todo"}, "category:work", "onDropItem");
    validZone.props.emplace("direction", std::string("row"));
    validZone.props.emplace("radius", 8.0);

    ui::UiTreeNode tree = makeNode("row");
    tree.children.push_back(validSource);
    tree.children.push_back(validZone);
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    ok = expect(source != nullptr && source->enabled(), "valid DragSource enabled") && ok;
    ok = expect(
             source != nullptr && source->dragType() == "keybind" && source->payload() == "bind:original",
             "valid DragSource props applied"
         )
        && ok;
    ok = expect(
             source != nullptr && source->inputArea() != nullptr && source->inputArea()->hasTooltip(),
             "DragSource tooltip applied"
         )
        && ok;
    ok = expect(
             zone != nullptr && zone->enabled() && zone->accepts("keybind") && zone->accepts("todo"),
             "valid DropZone accepts applied"
         )
        && ok;
    ok = expect(
             zone != nullptr
                 && zone->value() == "category:work"
                 && zone->onDrop() == "onDropItem"
                 && zone->direction() == FlexDirection::Horizontal,
             "valid DropZone value, callback and direction applied"
         )
        && ok;
    if (zone != nullptr) {
      const float previousCornerScale = Style::cornerRadiusScale();
      Style::setCornerRadiusScale(1.5f);
      (void)reconciler.reconcile(host, tree, renderer);
      zone->setDragOver(true);
      const auto background = std::ranges::find_if(zone->children(), [](const auto& child) {
        return dynamic_cast<const RectNode*>(child.get()) != nullptr;
      });
      const auto* rect =
          background != zone->children().end() ? dynamic_cast<const RectNode*>(background->get()) : nullptr;
      ok = expect(
               rect != nullptr && rect->style().radius == Style::scaledRadius(8.0f),
               "DropZone radius follows Noctalia corner roundness"
           )
          && ok;
      zone->setDragOver(false);
      Style::setCornerRadiusScale(previousCornerScale);
      (void)reconciler.reconcile(host, tree, renderer);
    }

    Node* sourceBefore = source;
    Node* zoneBefore = zone;

    const auto assertInvalidSource = [&](ui::UiTreeNode invalid, const char* message) {
      tree.children[0] = std::move(invalid);
      (void)reconciler.reconcile(host, tree, renderer);
      auto* current = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
      const bool invalidated = current == sourceBefore
          && current != nullptr
          && !current->enabled()
          && current->dragType().empty()
          && current->payload().empty();
      ok = expect(invalidated, message) && ok;
      tree.children[0] = validSource;
      (void)reconciler.reconcile(host, tree, renderer);
    };

    ui::UiTreeNode invalid = validSource;
    invalid.props.erase("dragType");
    assertInvalidSource(std::move(invalid), "missing source dragType clears retained contract");
    invalid = validSource;
    invalid.props["dragType"] = true;
    assertInvalidSource(std::move(invalid), "mistyped source dragType clears retained contract");
    invalid = validSource;
    invalid.props["dragType"] = std::string{};
    assertInvalidSource(std::move(invalid), "empty source dragType clears retained contract");
    invalid = validSource;
    invalid.props["dragType"] = std::string(257, 't');
    assertInvalidSource(std::move(invalid), "over-limit source dragType clears retained contract");
    invalid = validSource;
    invalid.props.erase("payload");
    assertInvalidSource(std::move(invalid), "missing source payload clears retained contract");
    invalid = validSource;
    invalid.props["payload"] = 1.0;
    assertInvalidSource(std::move(invalid), "mistyped source payload clears retained contract");
    invalid = validSource;
    invalid.props["payload"] = std::string{};
    assertInvalidSource(std::move(invalid), "empty source payload clears retained contract");
    invalid = validSource;
    invalid.props["payload"] = std::string(16 * 1024 + 1, 'p');
    assertInvalidSource(std::move(invalid), "over-limit source payload clears retained contract");
    invalid = validSource;
    invalid.props["enabled"] = std::string("yes");
    assertInvalidSource(std::move(invalid), "mistyped source enabled clears retained contract");

    const auto assertInvalidZone = [&](ui::UiTreeNode invalidZone, const char* message) {
      tree.children[1] = std::move(invalidZone);
      (void)reconciler.reconcile(host, tree, renderer);
      auto* current = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
      const bool invalidated = current == zoneBefore
          && current != nullptr
          && !current->enabled()
          && !current->accepts("keybind")
          && current->value().empty()
          && current->onDrop().empty();
      ok = expect(invalidated, message) && ok;
      tree.children[1] = validZone;
      (void)reconciler.reconcile(host, tree, renderer);
    };

    invalid = validZone;
    invalid.props.erase("value");
    assertInvalidZone(std::move(invalid), "missing zone value clears retained contract");
    invalid = validZone;
    invalid.props["value"] = false;
    assertInvalidZone(std::move(invalid), "mistyped zone value clears retained contract");
    invalid = validZone;
    invalid.props["value"] = std::string{};
    assertInvalidZone(std::move(invalid), "empty zone value clears retained contract");
    invalid = validZone;
    invalid.props["value"] = std::string(257, 'v');
    assertInvalidZone(std::move(invalid), "over-limit zone value clears retained contract");
    invalid = validZone;
    invalid.props.erase("onDrop");
    assertInvalidZone(std::move(invalid), "missing zone onDrop clears retained contract");
    invalid = validZone;
    invalid.props["onDrop"] = 2.0;
    assertInvalidZone(std::move(invalid), "mistyped zone onDrop clears retained contract");
    invalid = validZone;
    invalid.props["onDrop"] = std::string{};
    assertInvalidZone(std::move(invalid), "empty zone onDrop clears retained contract");
    invalid = validZone;
    invalid.props["onDrop"] = std::string(257, 'c');
    assertInvalidZone(std::move(invalid), "over-limit zone onDrop clears retained contract");
    invalid = validZone;
    invalid.props.erase("accepts");
    assertInvalidZone(std::move(invalid), "missing zone accepts clears retained contract");
    invalid = validZone;
    invalid.props["accepts"] = std::vector<double>{};
    assertInvalidZone(std::move(invalid), "mistyped zone accepts clears retained contract");
    invalid = validZone;
    invalid.props["accepts"] = std::vector<std::string>(17, "keybind");
    assertInvalidZone(std::move(invalid), "oversized zone accepts clears retained contract");
    invalid = validZone;
    invalid.props["accepts"] = std::vector<std::string>{"keybind", ""};
    assertInvalidZone(std::move(invalid), "empty zone accepts entry clears retained contract");
    invalid = validZone;
    invalid.props["accepts"] = std::vector<std::string>{std::string(257, 'a')};
    assertInvalidZone(std::move(invalid), "over-limit zone accepts entry clears retained contract");
    invalid = validZone;
    invalid.props["enabled"] = std::string("yes");
    assertInvalidZone(std::move(invalid), "mistyped zone enabled clears retained contract");

    ui::UiTreeNode emptyAccepts = validZone;
    emptyAccepts.props["accepts"] = std::vector<std::string>{};
    tree.children[1] = std::move(emptyAccepts);
    (void)reconciler.reconcile(host, tree, renderer);
    zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    ok = expect(zone == zoneBefore && zone != nullptr && zone->enabled(), "accepts={} is a valid retained DropZone")
        && ok;
    ok = expect(zone != nullptr && !zone->accepts("keybind"), "accepts={} accepts no drag type") && ok;

    tree.children[0] = validSource;
    tree.children[0].props["payload"] = std::string("bind:updated");
    tree.children[1] = validZone;
    tree.children[1].props["value"] = std::string("category:updated");
    (void)reconciler.reconcile(host, tree, renderer);
    source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    ok = expect(source == sourceBefore && zone == zoneBefore, "keyed DnD controls reused after prop changes") && ok;
    ok = expect(
             source != nullptr
                 && source->payload() == "bind:updated"
                 && zone != nullptr
                 && zone->value() == "category:updated",
             "keyed DnD controls receive updated props"
         )
        && ok;
  }

  // The retained controls drive the full drag lifecycle: threshold arming,
  // accepted/rejected hit testing, callback payloads, and pre-callback cleanup.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    std::vector<ui::UiTreeReconciler::ControlCallback> callbacks;
    bool callbackSawCleanState = false;
    DragSource* source = nullptr;
    DropZone* accepted = nullptr;
    DropZone* rejected = nullptr;
    DragDropController* controller = nullptr;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback& callback) {
      callbackSawCleanState = controller != nullptr
          && controller->state() == DragDropController::State::Idle
          && controller->activeSource() == nullptr
          && controller->currentTarget() == nullptr
          && source != nullptr
          && !source->dragging()
          && accepted != nullptr
          && !accepted->dragOver()
          && rejected != nullptr
          && !rejected->dragOver();
      callbacks.push_back(callback);
    });

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.props.emplace("align", std::string("start"));
    tree.children.push_back(makeDragSource("source", "keybind", "bind:42"));
    tree.children.back().props.emplace("tooltip", std::string("Move keybind"));
    tree.children.push_back(makeDropZone("accepted", {"keybind"}, "category:media", "onDropKeybind"));
    tree.children.push_back(makeDropZone("rejected", {"todo"}, "category:todo", "onDropTodo"));
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    accepted = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    rejected = row != nullptr ? dynamic_cast<DropZone*>(row->children()[2].get()) : nullptr;
    controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);
    ok = expect(
             source != nullptr && accepted != nullptr && rejected != nullptr && controller != nullptr,
             "DnD fixture built"
         )
        && ok;

    if (source != nullptr && source->inputArea() != nullptr && controller != nullptr) {
      InputArea* area = source->inputArea();
      ok = expect(
               area->width() == source->width()
                   && area->height() == source->height()
                   && area->width() > 0.0f
                   && area->height() > 0.0f,
               "DragSource input overlay follows measure/arrange bounds without a manual layout pass"
           )
          && ok;

      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      ok = expect(controller->state() == DragDropController::State::Armed, "source press arms drag") && ok;
      area->dispatchMotion(2.0f + Style::dragStartThreshold - 0.5f, 2.0f);
      ok = expect(controller->state() == DragDropController::State::Armed, "motion below threshold stays armed") && ok;
      ok = expect(!source->dragging() && callbacks.empty(), "below-threshold motion has no drag visual or callback")
          && ok;
      area->dispatchPress(2.0f + Style::dragStartThreshold - 0.5f, 2.0f, BTN_LEFT, false);
      ok = expect(controller->state() == DragDropController::State::Idle, "below-threshold release returns idle") && ok;
      ok = expect(callbacks.empty(), "below-threshold release is a no-op") && ok;

      float acceptedX = 0.0f;
      float acceptedY = 0.0f;
      sourceLocalPointFor(*source, *accepted, acceptedX, acceptedY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(acceptedX, acceptedY);
      ok = expect(controller->state() == DragDropController::State::Dragging, "threshold crossing starts dragging")
          && ok;
      ok = expect(source->dragging(), "active drag reduces source visual") && ok;
      ok = expect(!area->hasTooltip(), "active drag suppresses the source tooltip") && ok;
      ok = expect(controller->currentTarget() == accepted && accepted->dragOver(), "accepted target highlighted") && ok;
      area->dispatchPress(acceptedX, acceptedY, BTN_LEFT, false);
      ok = expect(area->hasTooltip(), "drag end restores the source tooltip") && ok;
      ok = expect(callbacks.size() == 1, "accepted release fires exactly one callback") && ok;
      if (callbacks.size() == 1) {
        ok = expect(callbacks[0].fn == "onDropKeybind", "drop callback name preserved") && ok;
        ok = expect(callbacks[0].arg1 == "bind:42", "drop callback receives source payload") && ok;
        ok = expect(callbacks[0].arg2 == "category:media", "drop callback receives target value") && ok;
        ok = expect(!callbacks[0].coalesce, "drop callback is non-coalesced") && ok;
      }
      ok = expect(callbackSawCleanState, "drag state and visuals clear before callback dispatch") && ok;

      callbacks.clear();
      callbackSawCleanState = false;
      float rejectedX = 0.0f;
      float rejectedY = 0.0f;
      sourceLocalPointFor(*source, *rejected, rejectedX, rejectedY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(rejectedX, rejectedY);
      ok = expect(controller->state() == DragDropController::State::Dragging, "rejected target still crosses threshold")
          && ok;
      ok = expect(
               controller->currentTarget() == nullptr && !rejected->dragOver(),
               "rejected type never targets or highlights"
           )
          && ok;
      area->dispatchPress(rejectedX, rejectedY, BTN_LEFT, false);
      ok = expect(callbacks.empty(), "release on rejected target fires no callback") && ok;
      ok = expect(
               controller->state() == DragDropController::State::Idle && !source->dragging(),
               "rejected release restores source and controller"
           )
          && ok;
    }
  }

  // hitSlop expands only drag targeting, not layout. A pointer can therefore
  // select a 3px insertion marker from the adjacent row without adding visible
  // whitespace or changing ordinary input hit testing.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback&) { ++callbackCount; });

    ui::UiTreeNode insertion = makeDropZone("insertion", {"keybind"}, "before:2", "onInsert");
    insertion.props["height"] = 3.0;
    insertion.props["hitSlop"] = 14.0;
    insertion.props["radius"] = 9.0;
    ui::UiTreeNode followingRow = makeNode("row");
    followingRow.props.emplace("width", 70.0);
    followingRow.props.emplace("height", 30.0);

    ui::UiTreeNode tree = makeDropZone("category", {"keybind"}, "category", "onCategory");
    tree.props["direction"] = std::string("column");
    tree.props["width"] = 100.0;
    tree.props["height"] = 100.0;
    tree.props.emplace("align", std::string("start"));
    tree.children.push_back(makeDragSource());
    tree.children.push_back(std::move(insertion));
    tree.children.push_back(std::move(followingRow));
    (void)reconciler.reconcile(host, tree, renderer);
    host.setSize(100.0f, 100.0f);
    host.layout(renderer);

    auto* column = dynamic_cast<DropZone*>(host.children().front().get());
    auto* source = column != nullptr ? dynamic_cast<DragSource*>(column->children()[0].get()) : nullptr;
    auto* zone = column != nullptr ? dynamic_cast<DropZone*>(column->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    const RectNode* zoneBackground = nullptr;
    if (zone != nullptr) {
      for (const auto& child : zone->children()) {
        if (auto* rect = dynamic_cast<RectNode*>(child.get())) {
          zoneBackground = rect;
          break;
        }
      }
    }
    const float expectedRadius = Style::scaledRadius(9.0f, 1.0f);
    ok = expect(
             source != nullptr
                 && zone != nullptr
                 && controller != nullptr
                 && zone->height() == 3.0f
                 && zone->hitSlop() == 14.0f,
             "drop hitSlop preserves the thin insertion marker layout"
         )
        && ok;
    ok = expect(
             zoneBackground != nullptr && zoneBackground->style().radius == Radii(expectedRadius),
             "drop zone applies its declared radius before the first drag"
         )
        && ok;

    if (source != nullptr && zone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointAt(*source, *zone, zone->width() - 2.0f, -10.0f, localX, localY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(localX, localY);
      ok = expect(
               controller->currentTarget() == zone && zone->dragOver(),
               "proximity insertion zone wins over its catch-all category ancestor"
           )
          && ok;
      source->inputArea()->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 1, "release inside hitSlop dispatches the insertion callback") && ok;
      ok = expect(
               zoneBackground != nullptr && zoneBackground->style().radius == Radii(expectedRadius),
               "drop zone preserves its declared radius after drag-over clears"
           )
          && ok;
    }
  }

  // Overlapping proximity zones turn a vertical list into a continuous
  // insertion surface. Crossing the physical midpoint of a row transfers the
  // single full-height placeholder to the next boundary; there is never a
  // frame where both boundaries are collapsed.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Node overlay;
    overlay.setSize(200.0f, 200.0f);
    overlay.setHitTestVisible(false);
    reconciler.setDragDropOverlayRoot(&overlay);
    Flex host;

    ui::UiTreeNode sourceRow = makeNode("row");
    sourceRow.key = "lifted-row";
    sourceRow.props.emplace("width", 70.0);
    sourceRow.props.emplace("height", 30.0);
    ui::UiTreeNode sourceNode = makeDragSource("continuous-source", "keybind", "bind:continuous");
    sourceNode.props.emplace("previewAncestor", 1.0);
    sourceNode.props.emplace("liftFromLayout", true);
    sourceRow.children.push_back(std::move(sourceNode));

    auto insertionNode = [](std::string key, std::string value) {
      ui::UiTreeNode zone = makeDropZone(std::move(key), {"keybind"}, std::move(value), "onContinuousDrop");
      zone.props["height"] = 3.0;
      zone.props["expandOnDrag"] = true;
      zone.props["hitSlop"] = 64.0;
      return zone;
    };
    ui::UiTreeNode middleRow = makeNode("row");
    middleRow.props.emplace("width", 70.0);
    middleRow.props.emplace("height", 30.0);
    ui::UiTreeNode lastRow = middleRow;

    ui::UiTreeNode tree = makeDropZone("continuous-category", {"keybind"}, "category", "onCategory");
    tree.props["direction"] = std::string("column");
    tree.props["width"] = 100.0;
    tree.props["height"] = 160.0;
    tree.props.emplace("align", std::string("start"));
    tree.children.push_back(std::move(sourceRow));
    tree.children.push_back(insertionNode("gap-before", "before:middle"));
    tree.children.push_back(std::move(middleRow));
    tree.children.push_back(insertionNode("gap-after", "after:middle"));
    tree.children.push_back(std::move(lastRow));
    (void)reconciler.reconcile(host, tree, renderer);
    host.setSize(160.0f, 200.0f);
    host.layout(renderer);

    auto* category = dynamic_cast<DropZone*>(host.children().front().get());
    auto* liftedRow = category != nullptr ? dynamic_cast<Flex*>(category->children()[0].get()) : nullptr;
    auto* source = liftedRow != nullptr ? dynamic_cast<DragSource*>(liftedRow->children()[0].get()) : nullptr;
    auto* before = category != nullptr ? dynamic_cast<DropZone*>(category->children()[1].get()) : nullptr;
    auto* middle = category != nullptr ? dynamic_cast<Flex*>(category->children()[2].get()) : nullptr;
    auto* after = category != nullptr ? dynamic_cast<DropZone*>(category->children()[3].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    ok = expect(
             source != nullptr && before != nullptr && middle != nullptr && after != nullptr && controller != nullptr,
             "continuous insertion fixture built"
         )
        && ok;

    if (source != nullptr
        && before != nullptr
        && middle != nullptr
        && after != nullptr
        && controller != nullptr
        && source->inputArea() != nullptr) {
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointAt(*source, *middle, middle->width() * 0.5f, middle->height() * 0.25f, localX, localY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(localX, localY);
      host.layout(renderer);
      ok = expect(
               controller->currentTarget() == before
                   && before->minHeight() == liftedRow->height()
                   && after->minHeight() == 3.0f,
               "upper row half keeps exactly the preceding full-height placeholder"
           )
          && ok;

      sourceLocalPointAt(*source, *middle, middle->width() * 0.5f, middle->height() * 0.75f, localX, localY);
      source->inputArea()->dispatchMotion(localX, localY);
      ok = expect(
               controller->currentTarget() == after
                   && before->minHeight() == 3.0f
                   && after->minHeight() == liftedRow->height(),
               "crossing row midpoint transfers the full-height placeholder"
           )
          && ok;
      source->inputArea()->dispatchPress(localX, localY, BTN_LEFT, false);
    }
  }

  // A bounded previewAncestor paints the selected ancestor into the dedicated
  // overlay only after the drag threshold. liftFromLayout removes the retained
  // row from layout while preserving it as the proxy source, then restores it
  // before the drop callback returns.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Node overlay;
    overlay.setSize(400.0f, 160.0f);
    overlay.setHitTestVisible(false);
    reconciler.setDragDropOverlayRoot(&overlay);
    Flex host;

    ui::UiTreeNode previewRow = makeNode("row");
    previewRow.key = "preview-row";
    previewRow.props.emplace("width", 140.0);
    previewRow.props.emplace("height", 30.0);
    ui::UiTreeNode sourceNode = makeDragSource("preview-source", "keybind", "bind:preview");
    sourceNode.props.emplace("previewAncestor", 1.0);
    sourceNode.props.emplace("liftFromLayout", true);
    previewRow.children.push_back(std::move(sourceNode));
    previewRow.children.push_back(makeLabel("shortcut row"));

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 30.0);
    tree.children.push_back(std::move(previewRow));
    ui::UiTreeNode previewZone = makeDropZone("preview-zone", {"keybind"}, "target", "onPreviewDrop");
    previewZone.props["height"] = 3.0;
    previewZone.props.emplace("expandOnDrag", true);
    tree.children.push_back(std::move(previewZone));
    (void)reconciler.reconcile(host, tree, renderer);
    host.setSize(400.0f, 160.0f);
    host.layout(renderer);

    auto* rootRow = dynamic_cast<Flex*>(host.children().front().get());
    auto* row = rootRow != nullptr ? dynamic_cast<Flex*>(rootRow->children()[0].get()) : nullptr;
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* zone = rootRow != nullptr ? dynamic_cast<DropZone*>(rootRow->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    ok = expect(
             row != nullptr && source != nullptr && zone != nullptr && controller != nullptr,
             "drag preview fixture built"
         )
        && ok;

    if (row != nullptr
        && source != nullptr
        && zone != nullptr
        && controller != nullptr
        && source->inputArea() != nullptr) {
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointFor(*source, *zone, localX, localY);
      const float originalOpacity = row->opacity();
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(2.0f + Style::dragStartThreshold - 0.5f, 2.0f);
      ok = expect(overlay.children().empty(), "preview is absent below drag threshold") && ok;

      source->inputArea()->dispatchMotion(localX, localY);
      auto* proxy =
          overlay.children().empty() ? nullptr : dynamic_cast<RenderProxyNode*>(overlay.children().front().get());
      ok = expect(
               proxy != nullptr && proxy->source() == row && controller->state() == DragDropController::State::Dragging,
               "drag threshold creates one ancestor render proxy"
           )
          && ok;
      ok = expect(
               row->opacity() == 0.0f && !row->participatesInLayout(),
               "liftFromLayout removes and hides the original row"
           )
          && ok;
      ok = expect(
               zone->minHeight() == row->height() && zone->maxHeight() == row->height(),
               "expandOnDrag reserves the dragged row height in native layout"
           )
          && ok;
      ok = expect(
               Node::hitTest(&overlay, proxy != nullptr ? proxy->x() : 0.0f, proxy != nullptr ? proxy->y() : 0.0f)
                   == nullptr,
               "drag preview overlay is excluded from hit testing"
           )
          && ok;

      source->inputArea()->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(overlay.children().empty(), "drop removes drag preview before callback rerender") && ok;
      ok = expect(
               row->opacity() == originalOpacity && row->participatesInLayout(),
               "drop restores the exact original row layout and opacity"
           )
          && ok;
      ok = expect(zone->minHeight() == 3.0f && zone->maxHeight() == 3.0f, "drop collapses insertion zone") && ok;
    }
  }

  // previewAncestor may request more levels than exist on the content branch.
  // Clamp before the common panel root because that root also owns the overlay;
  // using it as a proxy source would make the renderer encounter the proxy
  // recursively while painting its own source.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);

    Node panelRoot;
    panelRoot.setSize(400.0f, 160.0f);
    auto content = std::make_unique<Flex>();
    auto* contentHost = static_cast<Flex*>(panelRoot.addChild(std::move(content)));
    contentHost->setSize(400.0f, 160.0f);
    auto overlay = std::make_unique<Node>();
    overlay->setSize(400.0f, 160.0f);
    overlay->setHitTestVisible(false);
    auto* overlayRoot = panelRoot.addChild(std::move(overlay));
    reconciler.setDragDropOverlayRoot(overlayRoot);

    ui::UiTreeNode sourceNode = makeDragSource("bounded-source", "keybind", "bind:bounded");
    sourceNode.props["previewAncestor"] = 8.0;
    ui::UiTreeNode tree = makeNode("row");
    tree.children.push_back(std::move(sourceNode));
    (void)reconciler.reconcile(*contentHost, tree, renderer);
    contentHost->layout(renderer);

    auto* row = contentHost->children().empty() ? nullptr : dynamic_cast<Flex*>(contentHost->children().front().get());
    auto* source =
        row == nullptr || row->children().empty() ? nullptr : dynamic_cast<DragSource*>(row->children().front().get());
    auto* controller = source != nullptr ? source->controller() : nullptr;
    ok = expect(source != nullptr && controller != nullptr, "bounded preview fixture built") && ok;

    if (source != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(2.0f + Style::dragStartThreshold + 1.0f, 2.0f);
      auto* proxy = overlayRoot->children().empty()
          ? nullptr
          : dynamic_cast<RenderProxyNode*>(overlayRoot->children().front().get());
      ok = expect(
               proxy != nullptr && proxy->source() == contentHost && proxy->source() != &panelRoot,
               "preview clamps to the highest content-only ancestor"
           )
          && ok;
      bool proxySourceContainsOverlay = false;
      if (proxy != nullptr) {
        for (const Node* current = overlayRoot; current != nullptr; current = current->parent()) {
          if (current == proxy->source()) {
            proxySourceContainsOverlay = true;
            break;
          }
        }
      }
      ok = expect(!proxySourceContainsOverlay, "render proxy source never contains its overlay") && ok;
      controller->cancel();
      ok = expect(overlayRoot->children().empty(), "bounded preview cancellation removes the proxy") && ok;
    }
  }

  // Ancestor walking selects the deepest accepting nested zone. Empty space
  // between nested zones belongs to the accepting outer container instead.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    std::vector<std::string> targets;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback& callback) {
      targets.push_back(callback.arg2);
    });

    ui::UiTreeNode outer = makeDropZone("outer", {"keybind"}, "outer", "onNestedDrop");
    outer.props["width"] = 160.0;
    outer.props["height"] = 140.0;
    outer.props.emplace("padding", 10.0);
    outer.props.emplace("gap", 20.0);
    outer.children.push_back(makeDropZone("inner-a", {"keybind"}, "inner-a", "onNestedDrop"));
    outer.children.back().props["height"] = 30.0;
    outer.children.push_back(makeDropZone("inner-b", {"keybind"}, "inner-b", "onNestedDrop"));
    outer.children.back().props["height"] = 30.0;

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.props.emplace("align", std::string("start"));
    tree.children.push_back(makeDragSource());
    tree.children.push_back(std::move(outer));
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* outerZone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    auto* innerA = outerZone != nullptr ? dynamic_cast<DropZone*>(outerZone->children()[0].get()) : nullptr;
    auto* innerB = outerZone != nullptr ? dynamic_cast<DropZone*>(outerZone->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);
    ok = expect(
             source != nullptr
                 && outerZone != nullptr
                 && innerA != nullptr
                 && innerB != nullptr
                 && controller != nullptr,
             "nested DnD fixture built"
         )
        && ok;

    if (source != nullptr
        && outerZone != nullptr
        && innerA != nullptr
        && innerB != nullptr
        && controller != nullptr
        && source->inputArea() != nullptr) {
      InputArea* area = source->inputArea();
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointFor(*source, *innerA, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(
               controller->currentTarget() == innerA && innerA->dragOver() && !outerZone->dragOver(),
               "deepest accepting nested zone wins"
           )
          && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(targets.size() == 1 && targets.back() == "inner-a", "nested drop reports inner target value") && ok;

      const float gapTop = innerA->y() + innerA->height();
      const float gapBottom = innerB->y();
      ok = expect(gapBottom > gapTop, "nested fixture has real flex gap") && ok;
      sourceLocalPointAt(*source, *outerZone, outerZone->width() * 0.5f, (gapTop + gapBottom) * 0.5f, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(
               controller->currentTarget() == outerZone
                   && outerZone->dragOver()
                   && !innerA->dragOver()
                   && !innerB->dragOver(),
               "gap between inner zones resolves to outer zone"
           )
          && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(targets.size() == 2 && targets.back() == "outer", "gap drop reports outer target value") && ok;
    }
  }

  // Releases outside every zone or over a disabled zone cancel cleanly and do
  // not dispatch callbacks.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback&) { ++callbackCount; });

    ui::UiTreeNode disabled = makeDropZone("disabled", {"keybind"}, "disabled", "onDisabledDrop");
    disabled.props.emplace("enabled", false);
    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.children.push_back(makeDragSource());
    tree.children.push_back(std::move(disabled));
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* disabledZone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);

    if (source != nullptr && disabledZone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      InputArea* area = source->inputArea();
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointFor(*source, *disabledZone, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(
               controller->state() == DragDropController::State::Dragging
                   && controller->currentTarget() == nullptr
                   && !disabledZone->dragOver(),
               "disabled zone is never a target"
           )
          && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 0, "release on disabled zone fires no callback") && ok;

      sourceLocalPointAt(*source, host, host.width() - 2.0f, host.height() - 2.0f, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(
               controller->state() == DragDropController::State::Dragging && controller->currentTarget() == nullptr,
               "point outside zones has no target"
           )
          && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 0, "release outside zones fires no callback") && ok;
      ok = expect(
               controller->state() == DragDropController::State::Idle && !source->dragging(),
               "outside release restores controller and source visual"
           )
          && ok;
    }
  }

  // Scene hit testing ignores hidden zones and descendants outside a clipped
  // scroll viewport, while the visible portion of the same zone remains valid.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback&) { ++callbackCount; });

    ui::UiTreeNode clippedZone = makeDropZone("clipped-zone", {"keybind"}, "clipped", "onClippedDrop");
    clippedZone.props["width"] = 80.0;
    clippedZone.props["height"] = 100.0;
    clippedZone.props["hitSlop"] = 12.0;
    ui::UiTreeNode scroll = makeNode("scroll");
    scroll.key = "viewport";
    scroll.props.emplace("width", 100.0);
    scroll.props.emplace("height", 40.0);
    scroll.children.push_back(clippedZone);

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.props.emplace("align", std::string("start"));
    tree.children.push_back(makeDragSource());
    tree.children.push_back(std::move(scroll));
    (void)reconciler.reconcile(host, tree, renderer);

    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* scrollView = row != nullptr ? dynamic_cast<ScrollView*>(row->children()[1].get()) : nullptr;
    auto* zone = scrollView != nullptr && !scrollView->content()->children().empty()
        ? dynamic_cast<DropZone*>(scrollView->content()->children()[0].get())
        : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);
    if (scrollView != nullptr) {
      scrollView->setSize(100.0f, 40.0f);
      scrollView->layout(renderer);
    }

    if (source != nullptr && zone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      InputArea* area = source->inputArea();
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointAt(*source, *zone, 10.0f, 10.0f, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(controller->currentTarget() == zone, "visible part of zone inside scroll viewport is target") && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 1, "visible part of clipped zone accepts drop") && ok;

      sourceLocalPointAt(*source, *zone, 10.0f, 80.0f, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(controller->currentTarget() == nullptr, "zone portion outside scroll clip is not target") && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 1, "release outside scroll clip fires no callback") && ok;

      tree.children[1].children[0].props.emplace("visible", false);
      (void)reconciler.reconcile(host, tree, renderer);
      layoutDragTree(host, source, renderer);
      scrollView->setSize(100.0f, 40.0f);
      scrollView->layout(renderer);
      sourceLocalPointAt(*source, *zone, 10.0f, 10.0f, localX, localY);
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      ok = expect(
               controller->currentTarget() == nullptr,
               "visible=false zone is ignored by proximity and normal hit testing"
           )
          && ok;
      area->dispatchPress(localX, localY, BTN_LEFT, false);
      ok = expect(callbackCount == 1, "release on hidden zone fires no callback") && ok;
    }
  }

  // The drop state is cleared before the sink runs, so a sink may synchronously
  // replace and destroy the source subtree without a second callback or UAF.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    bool callbackSawIdle = false;
    bool rerenderChangedStructure = false;
    DragDropController* controller = nullptr;

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.children.push_back(makeDragSource("source", "keybind", "sync:payload"));
    tree.children.push_back(makeDropZone("zone", {"keybind"}, "sync:target", "onSyncDrop"));
    (void)reconciler.reconcile(host, tree, renderer);
    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);

    ui::UiTreeNode replacement = makeNode("column");
    replacement.key = "replacement";
    replacement.children.push_back(makeLabel("rerendered"));
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback& callback) {
      ++callbackCount;
      callbackSawIdle = callback.fn == "onSyncDrop"
          && callback.arg1 == "sync:payload"
          && callback.arg2 == "sync:target"
          && controller != nullptr
          && controller->state() == DragDropController::State::Idle
          && controller->activeSource() == nullptr
          && controller->currentTarget() == nullptr;
      rerenderChangedStructure = reconciler.reconcile(host, replacement, renderer);
    });

    if (source != nullptr && zone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      float localX = 0.0f;
      float localY = 0.0f;
      sourceLocalPointFor(*source, *zone, localX, localY);
      InputArea* area = source->inputArea();
      area->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      area->dispatchMotion(localX, localY);
      area->dispatchPress(localX, localY, BTN_LEFT, false);

      ok = expect(callbackCount == 1, "synchronous callback rerender fires exactly once") && ok;
      ok = expect(callbackSawIdle, "synchronous rerender callback observes cleared controller state") && ok;
      ok = expect(rerenderChangedStructure, "callback synchronously replaced source subtree") && ok;
      auto* replacementColumn = host.children().empty() ? nullptr : dynamic_cast<Flex*>(host.children().front().get());
      ok = expect(
               replacementColumn != nullptr
                   && replacementColumn->children().size() == 1
                   && dynamic_cast<Label*>(replacementColumn->children()[0].get()) != nullptr,
               "synchronous callback rerender leaves replacement tree alive"
           )
          && ok;
      ok = expect(
               controller->state() == DragDropController::State::Idle,
               "controller remains idle after synchronous rerender"
           )
          && ok;
    }
  }

  // Structural removal of a current target and reset() both cancel an active
  // drag, restore transient visuals, and never dispatch a drop callback.
  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback&) { ++callbackCount; });

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.children.push_back(makeDragSource());
    tree.children.push_back(makeDropZone());
    (void)reconciler.reconcile(host, tree, renderer);
    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);

    if (source != nullptr && zone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      float targetX = 0.0f;
      float targetY = 0.0f;
      sourceLocalPointFor(*source, *zone, targetX, targetY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(targetX, targetY);
      ok = expect(controller->currentTarget() == zone && zone->dragOver(), "removal test starts over target") && ok;

      DragSource* sourceBefore = source;
      DropZone* zoneBefore = zone;

      tree.children[1].props["accepts"] = std::vector<double>{};
      (void)reconciler.reconcile(host, tree, renderer);
      ok = expect(
               controller->state() == DragDropController::State::Idle
                   && !source->dragging()
                   && !zone->dragOver()
                   && !zone->enabled(),
               "invalidating current target props cancels active drag"
           )
          && ok;
      source->inputArea()->dispatchPress(targetX, targetY, BTN_LEFT, false);
      ok = expect(callbackCount == 0, "release after target prop invalidation cannot drop") && ok;

      tree.children[1] = makeDropZone();
      (void)reconciler.reconcile(host, tree, renderer);
      zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
      ok = expect(zone == zoneBefore && zone != nullptr && zone->enabled(), "valid target props restore keyed zone")
          && ok;
      layoutDragTree(host, source, renderer);
      sourceLocalPointFor(*source, *zone, targetX, targetY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(targetX, targetY);

      tree.children[0].props.erase("payload");
      (void)reconciler.reconcile(host, tree, renderer);
      ok = expect(
               controller->state() == DragDropController::State::Idle
                   && !source->dragging()
                   && !zone->dragOver()
                   && !source->enabled()
                   && source->payload().empty(),
               "invalidating active source props cancels active drag"
           )
          && ok;
      source->inputArea()->dispatchPress(targetX, targetY, BTN_LEFT, false);
      ok = expect(callbackCount == 0, "release after source prop invalidation cannot drop") && ok;

      tree.children[0] = makeDragSource();
      (void)reconciler.reconcile(host, tree, renderer);
      source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
      ok = expect(
               source == sourceBefore && source != nullptr && source->enabled(),
               "valid source props restore keyed source"
           )
          && ok;
      layoutDragTree(host, source, renderer);
      sourceLocalPointFor(*source, *zone, targetX, targetY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(targetX, targetY);
      ok = expect(controller->currentTarget() == zone && zone->dragOver(), "removal test restarts over target") && ok;

      tree.children.erase(tree.children.begin() + 1);
      (void)reconciler.reconcile(host, tree, renderer);
      source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
      ok = expect(source == sourceBefore, "source keyed instance survives target removal") && ok;
      ok = expect(
               controller->state() == DragDropController::State::Idle
                   && controller->currentTarget() == nullptr
                   && source != nullptr
                   && !source->dragging(),
               "target removal cancels and restores active drag"
           )
          && ok;
      if (source != nullptr && source->inputArea() != nullptr) {
        source->inputArea()->dispatchPress(targetX, targetY, BTN_LEFT, false);
      }
      ok = expect(callbackCount == 0, "release after target removal cannot drop") && ok;
    }
  }

  {
    ui::UiTreeReconciler reconciler;
    reconciler.setDragDropEnabled(true);
    Flex host;
    int callbackCount = 0;
    reconciler.setCallbackSink([&](const ui::UiTreeReconciler::ControlCallback&) { ++callbackCount; });

    ui::UiTreeNode tree = makeNode("row");
    tree.props.emplace("gap", 20.0);
    tree.children.push_back(makeDragSource());
    tree.children.push_back(makeDropZone());
    (void)reconciler.reconcile(host, tree, renderer);
    auto* row = dynamic_cast<Flex*>(host.children().front().get());
    auto* source = row != nullptr ? dynamic_cast<DragSource*>(row->children()[0].get()) : nullptr;
    auto* zone = row != nullptr ? dynamic_cast<DropZone*>(row->children()[1].get()) : nullptr;
    auto* controller = source != nullptr ? source->controller() : nullptr;
    layoutDragTree(host, source, renderer);

    if (source != nullptr && zone != nullptr && controller != nullptr && source->inputArea() != nullptr) {
      float targetX = 0.0f;
      float targetY = 0.0f;
      sourceLocalPointFor(*source, *zone, targetX, targetY);
      source->inputArea()->dispatchPress(2.0f, 2.0f, BTN_LEFT, true);
      source->inputArea()->dispatchMotion(targetX, targetY);
      ok = expect(
               controller->state() == DragDropController::State::Dragging && zone->dragOver(), "reset test starts drag"
           )
          && ok;
      reconciler.reset();
      ok = expect(
               controller->state() == DragDropController::State::Idle
                   && controller->activeSource() == nullptr
                   && controller->currentTarget() == nullptr
                   && !source->dragging()
                   && !zone->dragOver(),
               "reset cancels drag and clears transient visuals"
           )
          && ok;
      source->inputArea()->dispatchPress(targetX, targetY, BTN_LEFT, false);
      ok = expect(callbackCount == 0, "release after reset cannot drop") && ok;
    }
  }

  return ok ? 0 : 1;
}
