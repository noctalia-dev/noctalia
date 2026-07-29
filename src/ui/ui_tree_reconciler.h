#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Flex;
class InputArea;
class Node;
class Renderer;
class DragDropController;
enum class FontWeight : int;

namespace ui {

  struct UiTreeNode;

  // Maps a declarative UiTreeNode tree onto a retained tree of src/ui/controls/.
  // The single place plugin UI intent becomes controls: plugin code never sees a
  // Node. Diffs against the previous tree — children matched by `key` when set,
  // else by position+type; a matched control is updated in place, a mismatch
  // replaces the subtree. Mutates the scene graph, so callers must run it in the
  // Layout phase (e.g. from a doLayout hook).
  class UiTreeReconciler {
  public:
    // A control callback fired by user interaction. `fn` is the plugin-global
    // function name from the callback prop (e.g. button onClick = "openDetails").
    // `arg1`/`arg2` carry the control's value as strings (toggle "true"/"false",
    // slider "0.5", input text, select index + text); empty for value-less
    // callbacks like button clicks. onHover sends state in `arg1` and the node's
    // `key` in `arg2`, so one handler can serve a whole keyed list. `coalesce`
    // is set for high-frequency streams (slider drag, input typing) where only
    // the latest value matters.
    struct ControlCallback {
      explicit ControlCallback(
          std::string fnName, std::string firstArg = {}, std::string secondArg = {}, bool coalesceStream = false
      )
          : fn(std::move(fnName)), arg1(std::move(firstArg)), arg2(std::move(secondArg)), coalesce(coalesceStream) {}

      std::string fn;
      std::string arg1;
      std::string arg2;
      bool coalesce = false;
    };
    using CallbackSink = std::function<void(const ControlCallback& callback)>;
    // Resolves a tree-supplied path (e.g. image source) to an absolute path.
    using PathResolver = std::function<std::string(const std::string& path)>;
    // Receives the input area of a freshly created control whose `focus` prop
    // is true. The host decides how (and whether) to grant keyboard focus.
    using FocusRequestSink = std::function<void(InputArea* area)>;

    UiTreeReconciler();
    ~UiTreeReconciler();

    UiTreeReconciler(const UiTreeReconciler&) = delete;
    UiTreeReconciler& operator=(const UiTreeReconciler&) = delete;

    void setCallbackSink(CallbackSink sink) { m_sink = std::move(sink); }
    void setPathResolver(PathResolver resolver) { m_resolver = std::move(resolver); }
    void setFocusRequestSink(FocusRequestSink sink) { m_focusSink = std::move(sink); }
    // Content scale multiplied into size-like props (fonts, gaps, sizes, radii).
    void setScale(float scale);
    // Host text defaults for label/glyph props the tree leaves unset, so
    // declarative text matches the host's imperative text (e.g. the bar's
    // per-widget font family/weight). Empty family = renderer-global font.
    void setTextDefaults(std::string fontFamily, FontWeight fontWeight) {
      m_defaultFontFamily = std::move(fontFamily);
      m_defaultFontWeight = fontWeight;
    }
    // Compact control chrome for space-tight hosts (bar widgets): buttons drop
    // the settings-tier min-height/padding and hug their content instead.
    void setCompactControls(bool compact) { m_compactControls = compact; }
    void setDragDropEnabled(bool enabled);
    void setDragDropOverlayRoot(Node* root);

    // Reconciles `tree` as the single child of `host`. Props are (re)applied on
    // every call — setters are change-checked, and the scale may differ between
    // passes. Returns true when the retained structure changed.
    bool reconcile(Flex& host, const UiTreeNode& tree, Renderer& renderer);

    // Drops all retained slot state. Call when the host tree was destroyed and
    // rebuilt out from under the reconciler (e.g. a panel whose scene is torn
    // down on close and recreated on the next open) — the retained Node* would
    // otherwise dangle. The next reconcile() rebuilds from scratch.
    void reset();

  private:
    struct Slot;

    bool
    syncChildren(Node& parent, std::vector<Slot>& slots, const std::vector<UiTreeNode>& desired, Renderer& renderer);
    void applyProps(Slot& slot, const UiTreeNode& desired, Renderer& renderer);
    // onClick/onHover wiring shared by the InputArea-wrapped controls
    // (row/column/box/image).
    void syncWrapperCallbacks(Slot& slot, const UiTreeNode& desired, Node* node);
    // Hover is state the plugin mirrors, so every "true" owes a "false". The
    // dispatcher only delivers leave to areas still in the scene, so the
    // reconciler tracks which node is reporting hover and closes it itself when
    // that node is dropped, rewired, or reset away.
    void openHover(const std::string& name, const std::string& key, const Node* owner);
    void closeHover();
    void releaseHover(const Node* owner);
    [[nodiscard]] bool subtreeOwnsHover(const Slot& slot) const;
    [[nodiscard]] std::unique_ptr<Node> createControl(const UiTreeNode& desired);

    CallbackSink m_sink;
    PathResolver m_resolver;
    FocusRequestSink m_focusSink;
    float m_scale = 1.0f;
    std::string m_defaultFontFamily;
    FontWeight m_defaultFontWeight; // initialized in the ctor (opaque enum here)
    bool m_compactControls = false;
    bool m_dragDropEnabled = false;
    // The node currently reporting hover, with the callback and key it opened
    // with. Null owner when nothing is hovered. The owner is only ever compared,
    // never dereferenced: it identifies the slot whose hover is open.
    std::string m_hoveredCallback;
    std::string m_hoveredKey;
    const Node* m_hoveredOwner = nullptr;
    std::unique_ptr<DragDropController> m_dragDropController;
    std::vector<Slot> m_rootSlots;
  };

} // namespace ui
