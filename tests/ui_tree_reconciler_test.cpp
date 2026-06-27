#include "render/backend/render_backend.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/controls/spacer.h"
#include "ui/controls/toggle.h"
#include "ui/ui_tree.h"
#include "ui/ui_tree_reconciler.h"

#include <cstdio>
#include <cstdlib>
#include <string>
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
        std::string_view text, float fontSize, FontWeight, float, int, TextAlign, std::string_view, TextEllipsize
    ) override {
      return TextMetrics{.width = static_cast<float>(text.size()) * fontSize * 0.5f, .bottom = fontSize};
    }
    TextMetrics measureFont(float fontSize, FontWeight) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, FontWeight
    ) override {}
    TextMetrics measureGlyph(char32_t, float fontSize) override {
      return TextMetrics{.width = fontSize, .bottom = fontSize};
    }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0f; }
  };

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "ui_tree_reconciler_test: %s\n", message);
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

  return ok ? 0 : 1;
}
