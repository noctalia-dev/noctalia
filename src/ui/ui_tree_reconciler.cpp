#include "ui/ui_tree_reconciler.h"

#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/drag_source.h"
#include "ui/controls/drop_zone.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/graph.h"
#include "ui/controls/image.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/progress_bar.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/controls/spacer.h"
#include "ui/controls/toggle.h"
#include "ui/drag_drop_controller.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/ui_tree.h"

#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>
#include <unordered_set>
#include <utility>

namespace ui {
  namespace {

    constexpr Logger kLog("ui-tree");

    const double* numProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<double>(&it->second) : nullptr;
    }

    const std::string* strProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::string>(&it->second) : nullptr;
    }

    const bool* boolProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<bool>(&it->second) : nullptr;
    }

    const std::vector<double>* arrayProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::vector<double>>(&it->second) : nullptr;
    }

    const std::vector<std::string>* strArrayProp(const UiTreeNode& node, const char* key) {
      const auto it = node.props.find(key);
      return it != node.props.end() ? std::get_if<std::vector<std::string>>(&it->second) : nullptr;
    }

    constexpr std::size_t kDragPayloadMaxBytes = 16 * 1024;
    constexpr std::size_t kDragIdentifierMaxBytes = 256;
    constexpr std::size_t kDropAcceptsMaxEntries = 16;

    bool isDragDropType(std::string_view type) { return type == "drag_source" || type == "drop_zone"; }

    struct DragDropPropError {
      std::string prop;
      std::string reason;
    };

    std::optional<DragDropPropError>
    validateRequiredString(const UiTreeNode& node, const char* key, std::size_t maxBytes) {
      const auto it = node.props.find(key);
      if (it == node.props.end()) {
        return DragDropPropError{.prop = key, .reason = "missing"};
      }
      const auto* value = std::get_if<std::string>(&it->second);
      if (value == nullptr) {
        return DragDropPropError{.prop = key, .reason = "must be a string"};
      }
      if (value->empty()) {
        return DragDropPropError{.prop = key, .reason = "must not be empty"};
      }
      if (value->size() > maxBytes) {
        return DragDropPropError{.prop = key, .reason = "exceeds the size limit"};
      }
      return std::nullopt;
    }

    std::optional<DragDropPropError> validateDragDropProps(const UiTreeNode& node) {
      if (node.type == "drag_source") {
        if (auto error = validateRequiredString(node, "dragType", kDragIdentifierMaxBytes)) {
          return error;
        }
        if (auto error = validateRequiredString(node, "payload", kDragPayloadMaxBytes)) {
          return error;
        }
      } else if (node.type == "drop_zone") {
        if (auto error = validateRequiredString(node, "value", kDragIdentifierMaxBytes)) {
          return error;
        }
        if (auto error = validateRequiredString(node, "onDrop", kDragIdentifierMaxBytes)) {
          return error;
        }
        const auto acceptsIt = node.props.find("accepts");
        if (acceptsIt == node.props.end()) {
          return DragDropPropError{.prop = "accepts", .reason = "missing"};
        }
        const auto* accepts = std::get_if<std::vector<std::string>>(&acceptsIt->second);
        if (accepts == nullptr) {
          return DragDropPropError{.prop = "accepts", .reason = "must be a string array"};
        }
        if (accepts->size() > kDropAcceptsMaxEntries) {
          return DragDropPropError{.prop = "accepts", .reason = "has too many entries"};
        }
        for (const auto& accepted : *accepts) {
          if (accepted.empty()) {
            return DragDropPropError{.prop = "accepts", .reason = "contains an empty entry"};
          }
          if (accepted.size() > kDragIdentifierMaxBytes) {
            return DragDropPropError{.prop = "accepts", .reason = "contains an over-limit entry"};
          }
        }
      }
      if (node.props.contains("enabled") && boolProp(node, "enabled") == nullptr) {
        return DragDropPropError{.prop = "enabled", .reason = "must be a boolean"};
      }
      return std::nullopt;
    }

    // Optional-prop counterpart to validateDragDropProps: a present-but-mistyped
    // optional prop falls back to its default, but never silently.
    void warnMistypedOptionalProp(const UiTreeNode& node, const char* key, const char* expected) {
      if (node.props.contains(key)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' expects {}; default used", node.type,
            node.key.empty() ? "<unkeyed>" : node.key, key, expected
        );
      }
    }

    // Role token ("primary", "on_surface", …) with an optional alpha suffix
    // ("primary/0.6" → the role at 60% alpha, resolved live against the palette),
    // or hex ("#rrggbb[aa]"). Alpha is 0.0–1.0; hex carries its own alpha byte.
    std::optional<ColorSpec> parseColor(const UiTreeNode& node, const char* key) {
      const std::string* token = strProp(node, key);
      if (token == nullptr) {
        return std::nullopt;
      }
      std::string_view base = *token;
      float alpha = 1.0f;
      if (const auto slash = base.find('/'); slash != std::string_view::npos) {
        const std::string_view alphaText = base.substr(slash + 1);
        base = base.substr(0, slash);
        const auto* end = alphaText.data() + alphaText.size();
        if (const auto res = std::from_chars(alphaText.data(), end, alpha);
            res.ec != std::errc{} || res.ptr != end || alpha < 0.0f || alpha > 1.0f) {
          kLog.warn(
              "ui node '{}': invalid alpha '{}' in color '{}' for prop '{}' (expected 0.0-1.0)", node.type, alphaText,
              *token, key
          );
          return std::nullopt;
        }
      }
      if (auto role = colorRoleFromToken(base); role.has_value()) {
        return colorSpecFromRole(*role, alpha);
      }
      if (base.size() != token->size()) {
        kLog.warn("ui node '{}': alpha suffix requires a color role, got '{}' for prop '{}'", node.type, base, key);
        return std::nullopt;
      }
      Color fixed;
      if (tryParseHexColor(*token, fixed)) {
        return fixedColorSpec(fixed);
      }
      kLog.warn("ui node '{}': unknown color '{}' for prop '{}'", node.type, *token, key);
      return std::nullopt;
    }

    std::optional<FontWeight> parseFontWeight(const UiTreeNode& node) {
      const std::string* token = strProp(node, "fontWeight");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "thin")
        return FontWeight::Thin;
      if (*token == "light")
        return FontWeight::Light;
      if (*token == "normal")
        return FontWeight::Normal;
      if (*token == "medium")
        return FontWeight::Medium;
      if (*token == "semibold")
        return FontWeight::SemiBold;
      if (*token == "bold")
        return FontWeight::Bold;
      if (*token == "heavy")
        return FontWeight::Heavy;
      kLog.warn("ui node '{}': unknown fontWeight '{}'", node.type, *token);
      return std::nullopt;
    }

    std::optional<FlexAlign> parseAlign(const UiTreeNode& node) {
      const std::string* token = strProp(node, "align");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return FlexAlign::Start;
      if (*token == "center")
        return FlexAlign::Center;
      if (*token == "end")
        return FlexAlign::End;
      if (*token == "stretch")
        return FlexAlign::Stretch;
      kLog.warn("ui node '{}': unknown align '{}'", node.type, *token);
      return std::nullopt;
    }

    std::optional<FlexJustify> parseJustify(const UiTreeNode& node) {
      const std::string* token = strProp(node, "justify");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return FlexJustify::Start;
      if (*token == "center")
        return FlexJustify::Center;
      if (*token == "end")
        return FlexJustify::End;
      if (*token == "space_between")
        return FlexJustify::SpaceBetween;
      kLog.warn("ui node '{}': unknown justify '{}'", node.type, *token);
      return std::nullopt;
    }

    std::optional<TextAlign> parseTextAlign(const UiTreeNode& node) {
      const std::string* token = strProp(node, "textAlign");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "start")
        return TextAlign::Start;
      if (*token == "center")
        return TextAlign::Center;
      if (*token == "end")
        return TextAlign::End;
      kLog.warn("ui node '{}': unknown textAlign '{}'", node.type, *token);
      return std::nullopt;
    }

    std::optional<ButtonVariant> parseButtonVariant(const UiTreeNode& node) {
      const std::string* token = strProp(node, "variant");
      if (token == nullptr) {
        return std::nullopt;
      }
      if (*token == "default")
        return ButtonVariant::Default;
      if (*token == "primary")
        return ButtonVariant::Primary;
      if (*token == "secondary")
        return ButtonVariant::Secondary;
      if (*token == "destructive")
        return ButtonVariant::Destructive;
      if (*token == "outline")
        return ButtonVariant::Outline;
      if (*token == "ghost")
        return ButtonVariant::Ghost;
      kLog.warn("ui node '{}': unknown button variant '{}'", node.type, *token);
      return std::nullopt;
    }

    // Named control-height tier, in unscaled logical pixels. Callers scale it.
    // Distinct from ui.glyph's numeric `size`; use `height` for exact pixels.
    std::optional<float> parseControlSize(const UiTreeNode& node) {
      const std::string* token = strProp(node, "controlSize");
      if (token == nullptr) {
        if (node.props.contains("controlSize")) {
          kLog.warn(
              "ui node '{}': controlSize expects 'sm'/'md'/'lg', not a number - use 'height' for exact pixels",
              node.type
          );
        }
        return std::nullopt;
      }
      if (*token == "sm")
        return Style::controlHeightSm;
      if (*token == "md")
        return Style::controlHeight;
      if (*token == "lg")
        return Style::controlHeightLg;
      kLog.warn("ui node '{}': unknown controlSize '{}'", node.type, *token);
      return std::nullopt;
    }

    InputArea* inputAreaFromSlot(Node* node) { return dynamic_cast<InputArea*>(node); }

    // The Node that a control's children reconcile into. Flex containers host
    // their children directly; a wrapped Flex hosts them in its inner Flex; a
    // ScrollView hosts them in its inner content Flex. Any other control
    // returns nullptr — it cannot have children.
    Node* childContainer(const std::string& type, Node* node) {
      if (node == nullptr) {
        return nullptr;
      }
      if (type == "column" || type == "row") {
        if (auto* inputArea = inputAreaFromSlot(node)) {
          const auto& kids = inputArea->children();
          return kids.empty() ? nullptr : kids.front().get();
        }
        return node;
      }
      if (type == "drag_source" || type == "drop_zone") {
        return node;
      }
      if (type == "scroll") {
        return static_cast<ScrollView*>(node)->content();
      }
      return nullptr;
    }

    std::vector<float> toFloatSeries(const std::vector<double>& values) {
      std::vector<float> out;
      out.reserve(values.size());
      for (double v : values) {
        out.push_back(std::clamp(static_cast<float>(v), 0.0f, 1.0f));
      }
      return out;
    }

    template <typename Control> Control* controlFromSlot(Node* node) {
      if (node == nullptr) {
        return nullptr;
      }
      if (auto* control = dynamic_cast<Control*>(node)) {
        return control;
      }
      if (auto* inputArea = dynamic_cast<InputArea*>(node)) {
        const auto& kids = inputArea->children();
        if (!kids.empty()) {
          return dynamic_cast<Control*>(kids.front().get());
        }
      }
      return nullptr;
    }

    // A callback prop with an empty name counts as unset. Callback wiring is
    // change-detected against the slot's empty-string default, so an empty
    // name would never wire a handler — but it WOULD create a wrapper that
    // swallows clicks/hover meant for ancestors and sits in tab order dead.
    const std::string* callbackProp(const UiTreeNode& node, const char* key) {
      const std::string* name = strProp(node, key);
      return name != nullptr && !name->empty() ? name : nullptr;
    }

    // Box/image and row/column get an InputArea wrapper only when clickable (see
    // createControl). If a reconcile flips that need, the existing node can't
    // be reused — its structure no longer matches — so it must be rebuilt like
    // a type change.
    bool clickableWrapMismatch(const UiTreeNode& want, Node* node) {
      if (want.type != "box" && want.type != "image" && want.type != "row" && want.type != "column") {
        return false;
      }
      const bool wantsWrapper = callbackProp(want, "onClick") != nullptr || callbackProp(want, "onHover") != nullptr;
      const bool hasWrapper = inputAreaFromSlot(node) != nullptr;
      return wantsWrapper != hasWrapper;
    }

    // InputArea never self-sizes; box/image mirror explicit sizes, but a
    // content-sized flex needs its child measurement forwarded.
    class ClickWrap final : public InputArea {
    protected:
      LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
        if (!children().empty()) {
          const LayoutSize measured = children().front()->measure(renderer, constraints);
          setSize(measured.width, measured.height);
          return measured;
        }
        return InputArea::doMeasure(renderer, constraints);
      }

      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        setPosition(rect.x, rect.y);
        setSize(rect.width, rect.height);
        if (!children().empty()) {
          children().front()->arrange(
              renderer, LayoutRect{.x = 0.0f, .y = 0.0f, .width = rect.width, .height = rect.height}
          );
        }
      }
    };

    std::unique_ptr<Node> wrapClickable(std::unique_ptr<Node> control, bool acceptClicks) {
      auto inputArea = std::make_unique<ClickWrap>();
      // A hover-only wrapper must not swallow clicks meant for ancestors.
      inputArea->setAcceptedButtons(acceptClicks ? InputArea::buttonMask(BTN_LEFT) : 0);
      if (acceptClicks) {
        inputArea->setFocusable(true);
      }
      inputArea->addChild(std::move(control));
      return inputArea;
    }

    void wireWrapperActivation(InputArea* inputArea, const std::function<void()>& sink) {
      // A wrapper created hover-only starts with an empty button mask.
      inputArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      inputArea->setOnClick([sink](const InputArea::PointerData&) { sink(); });
      inputArea->setFocusable(true);
      inputArea->setOnKeyDown([sink](const InputArea::KeyData& key) {
        if (key.pressed && KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
          sink();
        }
      });
    }

    // Wrapper input handlers deviate from the retain-absent-props default:
    // when onClick/onHover disappears while the other callback keeps the
    // wrapper alive, the stale handler must go with it — a retained one would
    // leave an invisible node that swallows clicks and sits in tab order as a
    // keyboard-activatable ghost.
    void clearWrapperActivation(InputArea* inputArea) {
      inputArea->setAcceptedButtons(0);
      inputArea->setFocusable(false);
      inputArea->setOnClick(nullptr);
      inputArea->setOnKeyDown(nullptr);
      // setOnClick(fn) auto-selects the pointer cursor; drop it with the click.
      inputArea->setCursorShape(0);
    }

    void clearWrapperHover(InputArea* inputArea) {
      inputArea->setOnEnter(nullptr);
      inputArea->setOnLeave(nullptr);
    }

    // Known prop keys per control type — an unknown prop is a loud skip, not a
    // silent no-op, so typos in plugin code surface immediately.
    const std::unordered_set<std::string>& knownProps(const std::string& type) {
      static const std::unordered_set<std::string> kCommon = {"width", "height", "flexGrow", "opacity", "visible"};
      static const std::unordered_set<std::string> kFlex = {"width",     "height",  "flexGrow",    "opacity",
                                                            "visible",   "gap",     "padding",     "paddingH",
                                                            "paddingV",  "align",   "justify",     "fill",
                                                            "radius",    "border",  "borderWidth", "minWidth",
                                                            "minHeight", "onClick", "onHover"};
      static const std::unordered_set<std::string> kBox = {"width",       "height",   "flexGrow", "opacity",
                                                           "visible",     "fill",     "radius",   "border",
                                                           "borderWidth", "softness", "onClick",  "onHover"};
      static const std::unordered_set<std::string> kLabel = {"width",      "height",   "flexGrow", "opacity",
                                                             "visible",    "text",     "fontSize", "color",
                                                             "fontWeight", "maxWidth", "maxLines", "textAlign",
                                                             "fontFamily", "baseline"};
      static const std::unordered_set<std::string> kGlyph = {"width",   "height", "flexGrow", "opacity",
                                                             "visible", "name",   "size",     "color"};
      static const std::unordered_set<std::string> kImage = {"width",   "height",      "flexGrow", "opacity",
                                                             "visible", "path",        "radius",   "fit",
                                                             "border",  "borderWidth", "onClick",  "onHover"};
      static const std::unordered_set<std::string> kSeparator = {"width",   "height",  "flexGrow",
                                                                 "opacity", "visible", "thickness",
                                                                 "color",   "spacing", "orientation"};
      static const std::unordered_set<std::string> kProgress = {"width",    "height", "flexGrow", "opacity", "visible",
                                                                "progress", "fill",   "track",    "radius"};
      static const std::unordered_set<std::string> kButton = {"width",       "height",  "flexGrow",     "opacity",
                                                              "visible",     "text",    "glyph",        "fontSize",
                                                              "glyphSize",   "variant", "contentAlign", "enabled",
                                                              "selected",    "onClick", "onRightClick", "tooltip",
                                                              "controlSize", "onHover"};
      static const std::unordered_set<std::string> kGraph = {"width",   "height",    "flexGrow",   "opacity",
                                                             "visible", "values",    "values2",    "color",
                                                             "color2",  "lineWidth", "fillOpacity"};
      static const std::unordered_set<std::string> kInput = {"width",    "height",   "flexGrow",    "opacity",
                                                             "visible",  "value",    "placeholder", "fontSize",
                                                             "enabled",  "password", "multiline",   "focus",
                                                             "onChange", "onSubmit", "controlSize"};
      static const std::unordered_set<std::string> kSelect = {"width",       "height",   "flexGrow",      "opacity",
                                                              "visible",     "options",  "selectedIndex", "enabled",
                                                              "placeholder", "onChange", "controlSize"};
      static const std::unordered_set<std::string> kSlider = {"width",      "height",  "flexGrow", "opacity",
                                                              "visible",    "min",     "max",      "step",
                                                              "value",      "enabled", "onChange", "onDragEnd",
                                                              "controlSize"};
      static const std::unordered_set<std::string> kToggle = {"width",   "height",  "flexGrow", "opacity",
                                                              "visible", "checked", "enabled",  "onChange"};
      static const std::unordered_set<std::string> kScroll = {"width",       "height", "flexGrow", "opacity",
                                                              "visible",     "fill",   "radius",   "border",
                                                              "borderWidth", "gap",    "padding",  "paddingH",
                                                              "paddingV",    "align",  "justify"};
      static const std::unordered_set<std::string> kDragSource = {
          "width",   "height",   "flexGrow",    "opacity",         "visible",       "gap",
          "padding", "paddingH", "paddingV",    "align",           "justify",       "fill",
          "radius",  "border",   "borderWidth", "minWidth",        "minHeight",     "dragType",
          "payload", "enabled",  "tooltip",     "previewAncestor", "liftFromLayout"
      };
      static const std::unordered_set<std::string> kDropZone = {
          "width",     "height",  "flexGrow", "opacity", "visible",   "gap",     "padding",      "paddingH",
          "paddingV",  "align",   "justify",  "fill",    "radius",    "border",  "borderWidth",  "minWidth",
          "minHeight", "accepts", "value",    "onDrop",  "direction", "enabled", "expandOnDrag", "hitSlop"
      };

      if (type == "column" || type == "row") {
        return kFlex;
      }
      if (type == "box") {
        return kBox;
      }
      if (type == "label") {
        return kLabel;
      }
      if (type == "glyph") {
        return kGlyph;
      }
      if (type == "image") {
        return kImage;
      }
      if (type == "separator") {
        return kSeparator;
      }
      if (type == "progress") {
        return kProgress;
      }
      if (type == "button") {
        return kButton;
      }
      if (type == "graph") {
        return kGraph;
      }
      if (type == "input") {
        return kInput;
      }
      if (type == "select") {
        return kSelect;
      }
      if (type == "slider") {
        return kSlider;
      }
      if (type == "toggle") {
        return kToggle;
      }
      if (type == "scroll") {
        return kScroll;
      }
      if (type == "drag_source") {
        return kDragSource;
      }
      if (type == "drop_zone") {
        return kDropZone;
      }
      return kCommon;
    }

  } // namespace

  struct UiTreeReconciler::Slot {
    std::string type;
    std::string key;
    Node* node = nullptr;
    std::string callbackName;        // last-wired button onClick / control onChange target
    std::string rightCallbackName;   // last-wired button onRightClick target
    std::string hoverCallbackName;   // last-wired onHover target (button/box/image/row/column)
    std::string submitCallbackName;  // last-wired input onSubmit target
    std::string dragEndCallbackName; // last-wired slider onDragEnd target
    std::string imagePath;           // last-applied resolved image source
    float imageTargetSize = 0.0f;
    // Controlled-with-change-detection: a value-driven control (toggle/slider/
    // select) only re-applies its declared value when it differs from the last
    // applied one, so an async re-render never fights an optimistic local change.
    std::optional<double> lastScalar;
    bool seeded = false; // an uncontrolled input has had its initial value applied
    std::vector<Slot> children;
  };

  UiTreeReconciler::UiTreeReconciler()
      : m_defaultFontWeight(FontWeight::Normal), m_dragDropController(std::make_unique<DragDropController>()) {
    m_dragDropController->setDropCallback([this](std::string callback, std::string payload, std::string target) {
      if (m_sink) {
        m_sink(ControlCallback{std::move(callback), std::move(payload), std::move(target), false});
      }
    });
  }

  UiTreeReconciler::~UiTreeReconciler() { reset(); }

  void UiTreeReconciler::setScale(float scale) {
    m_scale = scale;
    m_dragDropController->setScale(scale);
  }

  void UiTreeReconciler::setDragDropEnabled(bool enabled) {
    if (m_dragDropEnabled == enabled) {
      return;
    }
    if (!enabled) {
      m_dragDropController->cancel();
    }
    m_dragDropEnabled = enabled;
  }

  void UiTreeReconciler::setDragDropOverlayRoot(Node* root) { m_dragDropController->setOverlayRoot(root); }

  bool UiTreeReconciler::reconcile(Flex& host, const UiTreeNode& tree, Renderer& renderer) {
    std::vector<UiTreeNode> desired;
    desired.push_back(tree); // single root child of the host container
    return syncChildren(host, m_rootSlots, desired, renderer);
  }

  void UiTreeReconciler::reset() {
    m_dragDropController->cancel();
    // The host tree is gone, so any hover it was reporting has ended. The slots
    // still name it; the Nodes they point at are already freed.
    closeHover();
    m_rootSlots.clear();
  }

  std::unique_ptr<Node> UiTreeReconciler::createControl(const UiTreeNode& desired) {
    if (isDragDropType(desired.type) && !m_dragDropEnabled) {
      kLog.error("ui tree: '{}' is only supported in plugin panels; node skipped", desired.type);
      return nullptr;
    }
    // ui.* flex containers default to stretching children across the cross axis
    // (like CSS flexbox), so a column fills its width without every plugin having
    // to say so. Override per node with `align`. (Flex's own C++ default is
    // Center, which the native shell relies on — only the ui.* layer changes.)
    if (desired.type == "column") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Vertical);
      flex->setAlign(FlexAlign::Stretch);
      if (callbackProp(desired, "onClick") != nullptr || callbackProp(desired, "onHover") != nullptr) {
        return wrapClickable(std::move(flex), callbackProp(desired, "onClick") != nullptr);
      }
      return flex;
    }
    if (desired.type == "row") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Horizontal);
      flex->setAlign(FlexAlign::Stretch);
      if (callbackProp(desired, "onClick") != nullptr || callbackProp(desired, "onHover") != nullptr) {
        return wrapClickable(std::move(flex), callbackProp(desired, "onClick") != nullptr);
      }
      return flex;
    }
    if (desired.type == "drag_source") {
      auto source = std::make_unique<DragSource>(m_dragDropController.get());
      source->setDirection(FlexDirection::Horizontal);
      source->setAlign(FlexAlign::Stretch);
      return source;
    }
    if (desired.type == "drop_zone") {
      auto zone = std::make_unique<DropZone>(m_dragDropController.get());
      zone->setDirection(FlexDirection::Vertical);
      zone->setAlign(FlexAlign::Stretch);
      return zone;
    }
    if (desired.type == "box") {
      if (callbackProp(desired, "onClick") != nullptr || callbackProp(desired, "onHover") != nullptr) {
        return wrapClickable(std::make_unique<Box>(), callbackProp(desired, "onClick") != nullptr);
      }
      return std::make_unique<Box>();
    }
    if (desired.type == "label") {
      return std::make_unique<Label>();
    }
    if (desired.type == "glyph") {
      return std::make_unique<Glyph>();
    }
    if (desired.type == "image") {
      if (callbackProp(desired, "onClick") != nullptr || callbackProp(desired, "onHover") != nullptr) {
        return wrapClickable(std::make_unique<Image>(), callbackProp(desired, "onClick") != nullptr);
      }
      return std::make_unique<Image>();
    }
    if (desired.type == "separator") {
      return std::make_unique<Separator>();
    }
    if (desired.type == "spacer") {
      return std::make_unique<Spacer>();
    }
    if (desired.type == "progress") {
      return std::make_unique<ProgressBar>();
    }
    if (desired.type == "button") {
      return std::make_unique<Button>();
    }
    if (desired.type == "graph") {
      return std::make_unique<Graph>();
    }
    if (desired.type == "input") {
      return std::make_unique<Input>();
    }
    if (desired.type == "select") {
      return std::make_unique<Select>();
    }
    if (desired.type == "slider") {
      return std::make_unique<Slider>();
    }
    if (desired.type == "toggle") {
      return std::make_unique<Toggle>();
    }
    if (desired.type == "scroll") {
      auto scroll = std::make_unique<ScrollView>();
      scroll->content()->setAlign(FlexAlign::Stretch); // match the ui.* column/row default
      return scroll;
    }
    kLog.warn("ui tree: unknown control type '{}', node skipped", desired.type);
    return nullptr;
  }

  bool UiTreeReconciler::syncChildren(
      Node& parent, std::vector<Slot>& slots, const std::vector<UiTreeNode>& desired, Renderer& renderer
  ) {
    // Fast path: the (type, key) sequence is unchanged — update props in place.
    bool sequenceMatches = slots.size() == desired.size();
    if (sequenceMatches) {
      for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].type != desired[i].type
            || slots[i].key != desired[i].key
            || clickableWrapMismatch(desired[i], slots[i].node)
            || (isDragDropType(desired[i].type) && !m_dragDropEnabled)) {
          sequenceMatches = false;
          break;
        }
      }
    }

    bool structureChanged = false;
    if (!sequenceMatches) {
      structureChanged = true;
      // Detach the current children, then re-add in desired order, reusing a
      // detached control whose (type, key) matches; everything else is created
      // fresh and unmatched leftovers are dropped.
      struct Detached {
        Slot slot;
        std::unique_ptr<Node> node;
        bool used = false;
      };
      std::vector<Detached> detached;
      detached.reserve(slots.size());
      for (auto& slot : slots) {
        if (slot.node != nullptr) {
          m_dragDropController->cancelIfParticipantIn(slot.node);
          auto owned = parent.removeChild(slot.node);
          detached.push_back(Detached{.slot = std::move(slot), .node = std::move(owned)});
        }
      }
      slots.clear();
      slots.reserve(desired.size());

      for (const auto& want : desired) {
        Detached* match = nullptr;
        for (auto& candidate : detached) {
          if (candidate.used
              || (isDragDropType(want.type) && !m_dragDropEnabled)
              || candidate.slot.type != want.type
              || candidate.slot.key != want.key
              || clickableWrapMismatch(want, candidate.node.get())) {
            continue;
          }
          match = &candidate;
          break;
        }
        if (match != nullptr) {
          match->used = true;
          Slot slot = std::move(match->slot);
          slot.node = parent.addChild(std::move(match->node));
          slots.push_back(std::move(slot));
          continue;
        }

        auto control = createControl(want);
        if (control == nullptr) {
          continue;
        }
        Slot slot;
        slot.type = want.type;
        slot.key = want.key;
        slot.node = parent.addChild(std::move(control));
        slots.push_back(std::move(slot));
      }

      for (const auto& leftover : detached) {
        if (!leftover.used && subtreeOwnsHover(leftover.slot)) {
          closeHover();
          break;
        }
      }
    }

    // Apply props and recurse. With a sequence mismatch slots were rebuilt above
    // and may be shorter than `desired` (unknown types skipped).
    std::size_t slotIndex = 0;
    for (const auto& want : desired) {
      if (slotIndex >= slots.size()) {
        break;
      }
      Slot& slot = slots[slotIndex];
      if (slot.type != want.type || slot.key != want.key) {
        continue; // skipped unknown type
      }
      ++slotIndex;
      applyProps(slot, want, renderer);
      Node* container = childContainer(want.type, slot.node);
      if (slot.node != nullptr && !want.children.empty()) {
        if (container == nullptr) {
          kLog.warn("ui tree: '{}' cannot have children, {} dropped", want.type, want.children.size());
        } else {
          structureChanged |= syncChildren(*container, slot.children, want.children, renderer);
        }
      } else if (container != nullptr && want.children.empty() && !slot.children.empty()) {
        structureChanged |= syncChildren(*container, slot.children, {}, renderer);
      }
    }
    return structureChanged;
  }

  void UiTreeReconciler::syncWrapperCallbacks(Slot& slot, const UiTreeNode& desired, Node* node) {
    InputArea* inputArea = inputAreaFromSlot(node);
    if (const std::string* onClick = callbackProp(desired, "onClick"); onClick != nullptr) {
      if (*onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        if (inputArea != nullptr) {
          wireWrapperActivation(inputArea, [this, name = slot.callbackName]() {
            if (m_sink) {
              m_sink(ControlCallback{name});
            }
          });
        }
      }
    } else if (!slot.callbackName.empty()) {
      slot.callbackName.clear();
      if (inputArea != nullptr) {
        clearWrapperActivation(inputArea);
      }
    }
    if (const std::string* onHover = callbackProp(desired, "onHover"); onHover != nullptr) {
      if (*onHover != slot.hoverCallbackName) {
        releaseHover(node);
        slot.hoverCallbackName = *onHover;
        if (inputArea != nullptr) {
          inputArea->setOnEnter([this, name = slot.hoverCallbackName, key = desired.key,
                                 node](const InputArea::PointerData&) { openHover(name, key, node); });
          inputArea->setOnLeave([this, node]() { releaseHover(node); });
        }
      }
    } else if (!slot.hoverCallbackName.empty()) {
      releaseHover(node);
      slot.hoverCallbackName.clear();
      if (inputArea != nullptr) {
        clearWrapperHover(inputArea);
      }
    }
  }

  // Hover is state the plugin mirrors, so every "true" owes a "false". Exactly
  // one InputArea is hovered at a time, so tracking the one node that opened the
  // hover is enough to close it when that node is dropped, rewired, or reset
  // away. None of those reach the dispatcher's leave path, which only tracks
  // areas still in the scene.
  void UiTreeReconciler::openHover(const std::string& name, const std::string& key, const Node* owner) {
    if (m_hoveredOwner == owner) {
      return;
    }
    closeHover();
    m_hoveredCallback = name;
    m_hoveredKey = key;
    m_hoveredOwner = owner;
    if (m_sink) {
      m_sink(ControlCallback{name, "true", key});
    }
  }

  void UiTreeReconciler::closeHover() {
    if (m_hoveredOwner == nullptr) {
      return;
    }
    const std::string name = std::exchange(m_hoveredCallback, std::string{});
    const std::string key = std::exchange(m_hoveredKey, std::string{});
    m_hoveredOwner = nullptr;
    if (m_sink) {
      m_sink(ControlCallback{name, "false", key});
    }
  }

  // Closes the hover only if `owner` is the node currently reporting it, so
  // sibling nodes sharing one callback name do not close each other's hover.
  void UiTreeReconciler::releaseHover(const Node* owner) {
    if (owner != nullptr && owner == m_hoveredOwner) {
      closeHover();
    }
  }

  bool UiTreeReconciler::subtreeOwnsHover(const Slot& slot) const {
    if (m_hoveredOwner == nullptr) {
      return false;
    }
    if (slot.node == m_hoveredOwner) {
      return true;
    }
    for (const Slot& child : slot.children) {
      if (subtreeOwnsHover(child)) {
        return true;
      }
    }
    return false;
  }

  void UiTreeReconciler::applyProps(Slot& slot, const UiTreeNode& desired, Renderer& renderer) {
    Node* node = slot.node;
    if (node == nullptr) {
      return;
    }

    const auto& known = knownProps(desired.type);
    for (const auto& [key, value] : desired.props) {
      (void)value;
      if (!known.contains(key)) {
        kLog.warn("ui tree: '{}' has no prop '{}', ignored", desired.type, key);
      }
    }

    const auto scaled = [this](double v) { return static_cast<float>(v) * m_scale; };

    // Common node props.
    if (const bool* visible = boolProp(desired, "visible")) {
      if (!*visible) {
        m_dragDropController->cancelIfParticipantIn(node);
      }
      node->setVisible(*visible);
    }
    if (const double* opacity = numProp(desired, "opacity")) {
      const float clamped = std::clamp(static_cast<float>(*opacity), 0.0f, 1.0f);
      if (desired.type == "drag_source") {
        static_cast<DragSource*>(node)->setSourceOpacity(clamped);
      } else {
        node->setOpacity(clamped);
      }
    }
    if (const double* grow = numProp(desired, "flexGrow")) {
      node->setFlexGrow(static_cast<float>(*grow));
    }

    const double* width = numProp(desired, "width");
    const double* height = numProp(desired, "height");

    if (desired.type == "drag_source") {
      auto* source = static_cast<DragSource*>(node);
      if (auto error = validateDragDropProps(desired)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' {}; control disabled", desired.type,
            desired.key.empty() ? "<unkeyed>" : desired.key, error->prop, error->reason
        );
        source->setDragType({});
        source->setPayload({});
        source->setEnabled(false);
      } else {
        source->setDragType(*strProp(desired, "dragType"));
        source->setPayload(*strProp(desired, "payload"));
        source->setEnabled(boolProp(desired, "enabled") == nullptr || *boolProp(desired, "enabled"));
      }
      const std::string* tooltip = strProp(desired, "tooltip");
      if (tooltip == nullptr) {
        warnMistypedOptionalProp(desired, "tooltip", "a string");
      }
      source->setTooltip(tooltip != nullptr ? *tooltip : "");
      const double* previewAncestor = numProp(desired, "previewAncestor");
      if (previewAncestor == nullptr) {
        warnMistypedOptionalProp(desired, "previewAncestor", "an integer from 0 to 8");
        source->setPreviewAncestor(0);
      } else if (*previewAncestor < 0.0 || *previewAncestor > 8.0 || std::floor(*previewAncestor) != *previewAncestor) {
        kLog.warn("ui tree: 'drag_source' key '{}': previewAncestor expects an integer from 0 to 8", desired.key);
        source->setPreviewAncestor(0);
      } else {
        source->setPreviewAncestor(static_cast<std::size_t>(*previewAncestor));
      }
      const bool* liftFromLayout = boolProp(desired, "liftFromLayout");
      if (liftFromLayout == nullptr) {
        warnMistypedOptionalProp(desired, "liftFromLayout", "a boolean");
      }
      source->setLiftFromLayout(liftFromLayout != nullptr && *liftFromLayout);
    } else if (desired.type == "drop_zone") {
      auto* zone = static_cast<DropZone*>(node);
      if (auto error = validateDragDropProps(desired)) {
        kLog.warn(
            "ui tree: '{}' key '{}': prop '{}' {}; control disabled", desired.type,
            desired.key.empty() ? "<unkeyed>" : desired.key, error->prop, error->reason
        );
        zone->setAccepts({});
        zone->setValue({});
        zone->setOnDrop({});
        zone->setEnabled(false);
      } else {
        zone->setAccepts(*strArrayProp(desired, "accepts"));
        zone->setValue(*strProp(desired, "value"));
        zone->setOnDrop(*strProp(desired, "onDrop"));
        zone->setEnabled(boolProp(desired, "enabled") == nullptr || *boolProp(desired, "enabled"));
      }
      const bool* expandOnDrag = boolProp(desired, "expandOnDrag");
      if (expandOnDrag == nullptr) {
        warnMistypedOptionalProp(desired, "expandOnDrag", "a boolean");
      }
      zone->setExpandOnDrag(expandOnDrag != nullptr && *expandOnDrag);
      const double* hitSlop = numProp(desired, "hitSlop");
      if (hitSlop == nullptr) {
        warnMistypedOptionalProp(desired, "hitSlop", "a number");
      }
      zone->setHitSlop(hitSlop != nullptr ? scaled(std::max(0.0, *hitSlop)) : 0.0f);
    }

    if (desired.type == "column"
        || desired.type == "row"
        || desired.type == "drag_source"
        || desired.type == "drop_zone") {
      auto* flex = controlFromSlot<Flex>(node);
      if (flex == nullptr) {
        return;
      }
      if (desired.type == "drop_zone") {
        auto* zone = static_cast<DropZone*>(node);
        const std::string* direction = strProp(desired, "direction");
        if (direction == nullptr) {
          warnMistypedOptionalProp(desired, "direction", R"("column" or "row")");
          zone->setDirection(FlexDirection::Vertical);
        } else if (*direction == "column") {
          zone->setDirection(FlexDirection::Vertical);
        } else if (*direction == "row") {
          zone->setDirection(FlexDirection::Horizontal);
        } else {
          kLog.warn("ui tree: 'drop_zone' key '{}': unknown direction '{}'", desired.key, *direction);
          zone->setDirection(FlexDirection::Vertical);
        }
      }
      if (const double* gap = numProp(desired, "gap")) {
        flex->setGap(scaled(*gap));
      }
      const double* padding = numProp(desired, "padding");
      const double* paddingV = numProp(desired, "paddingV");
      const double* paddingH = numProp(desired, "paddingH");
      if (padding != nullptr || paddingV != nullptr || paddingH != nullptr) {
        const float fallback = padding != nullptr ? scaled(*padding) : 0.0f;
        flex->setPadding(
            paddingV != nullptr ? scaled(*paddingV) : fallback, paddingH != nullptr ? scaled(*paddingH) : fallback
        );
      }
      if (auto align = parseAlign(desired)) {
        flex->setAlign(*align);
      }
      if (auto justify = parseJustify(desired)) {
        flex->setJustify(*justify);
      }
      if (auto fill = parseColor(desired, "fill")) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneFill(*fill);
        } else {
          flex->setFill(*fill);
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneFill();
      }
      if (const double* radius = numProp(desired, "radius")) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneRadius(Style::scaledRadius(static_cast<float>(*radius), m_scale));
        } else {
          flex->setRadius(scaled(*radius));
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneRadius(Style::scaledRadiusSm(m_scale));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setZoneBorder(
              *border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth
          );
        } else {
          flex->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth);
        }
      } else if (desired.type == "drop_zone") {
        static_cast<DropZone*>(node)->clearZoneBorder();
      }
      if (const double* minWidth = numProp(desired, "minWidth")) {
        flex->setMinWidth(scaled(*minWidth));
      }
      if (const double* minHeight = numProp(desired, "minHeight")) {
        flex->setMinHeight(scaled(*minHeight));
      }
      if (width != nullptr) {
        flex->setMinWidth(scaled(*width));
        flex->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        if (desired.type == "drop_zone") {
          static_cast<DropZone*>(node)->setCollapsedHeight(scaled(*height));
        } else {
          flex->setMinHeight(scaled(*height));
          flex->setMaxHeight(scaled(*height));
        }
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "box") {
      auto* box = controlFromSlot<Box>(node);
      if (box == nullptr) {
        return;
      }
      if (auto fill = parseColor(desired, "fill")) {
        box->setFill(*fill);
      }
      if (const double* radius = numProp(desired, "radius")) {
        box->setRadius(scaled(*radius));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        box->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : 1.0f);
      }
      if (const double* softness = numProp(desired, "softness")) {
        box->setSoftness(static_cast<float>(*softness));
      }
      const float boxWidth = width != nullptr ? scaled(*width) : node->width();
      const float boxHeight = height != nullptr ? scaled(*height) : node->height();
      box->setSize(boxWidth, boxHeight);
      if (auto* inputArea = inputAreaFromSlot(node)) {
        inputArea->setSize(boxWidth, boxHeight);
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "label") {
      auto* label = static_cast<Label*>(node);
      if (const std::string* fontFamily = strProp(desired, "fontFamily")) {
        label->setFontFamily(*fontFamily);
      } else {
        label->setFontFamily(m_defaultFontFamily);
      }
      LabelBaselineMode baselineMode = LabelBaselineMode::Text;
      if (const std::string* baseline = strProp(desired, "baseline")) {
        if (auto mode = labelBaselineModeFromToken(*baseline)) {
          baselineMode = *mode;
        } else {
          kLog.warn("ui node 'label': unknown baseline '{}'", *baseline);
        }
      }
      label->setBaselineMode(baselineMode);
      if (const std::string* text = strProp(desired, "text")) {
        label->setText(*text);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        label->setFontSize(scaled(*fontSize));
      } else {
        label->setFontSize(Style::fontSizeBody * m_scale);
      }
      if (auto color = parseColor(desired, "color")) {
        label->setColor(*color);
      }
      if (auto weight = parseFontWeight(desired)) {
        label->setFontWeight(*weight);
      } else {
        label->setFontWeight(m_defaultFontWeight);
      }
      if (const double* maxWidth = numProp(desired, "maxWidth")) {
        label->setMaxWidth(scaled(*maxWidth));
      }
      if (const double* maxLines = numProp(desired, "maxLines")) {
        label->setMaxLines(static_cast<int>(*maxLines));
      }
      if (auto align = parseTextAlign(desired)) {
        label->setTextAlign(*align);
      }
      return;
    }

    if (desired.type == "glyph") {
      auto* glyph = static_cast<Glyph*>(node);
      if (const std::string* name = strProp(desired, "name")) {
        glyph->setGlyph(*name);
      }
      if (const double* size = numProp(desired, "size")) {
        glyph->setGlyphSize(scaled(*size));
      } else {
        glyph->setGlyphSize(Style::baseGlyphSize * m_scale);
      }
      if (auto color = parseColor(desired, "color")) {
        glyph->setColor(*color);
      }
      return;
    }

    if (desired.type == "image") {
      auto* image = controlFromSlot<Image>(node);
      if (image == nullptr) {
        return;
      }
      if (const double* radius = numProp(desired, "radius")) {
        image->setRadius(scaled(*radius));
      }
      if (const std::string* fit = strProp(desired, "fit")) {
        if (*fit == "contain") {
          image->setFit(ImageFit::Contain);
        } else if (*fit == "cover") {
          image->setFit(ImageFit::Cover);
        } else if (*fit == "stretch") {
          image->setFit(ImageFit::Stretch);
        } else {
          kLog.warn("ui tree: image has unknown fit '{}'", *fit);
        }
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        image->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : Style::borderWidth);
      } else {
        image->setBorder(clearColorSpec(), 0.0f);
      }
      const float imageWidth = width != nullptr ? scaled(*width) : node->width();
      const float imageHeight = height != nullptr ? scaled(*height) : imageWidth;
      image->setSize(imageWidth, imageHeight);
      if (auto* inputArea = inputAreaFromSlot(node)) {
        inputArea->setSize(imageWidth, imageHeight);
      }
      if (const std::string* path = strProp(desired, "path")) {
        const std::string resolved = m_resolver ? m_resolver(*path) : *path;
        const float targetSize = std::max(1.0f, std::max(imageWidth, imageHeight) * 3.0f);
        if (resolved != slot.imagePath || targetSize > slot.imageTargetSize) {
          slot.imagePath = resolved;
          slot.imageTargetSize = targetSize;
          if (!image->setSourceFile(renderer, resolved, static_cast<int>(std::round(targetSize)), true)) {
            kLog.warn("ui tree: image failed to load '{}'", resolved);
          }
        }
      }
      syncWrapperCallbacks(slot, desired, node);
      return;
    }

    if (desired.type == "separator") {
      auto* separator = static_cast<Separator*>(node);
      if (const double* thickness = numProp(desired, "thickness")) {
        separator->setThickness(scaled(*thickness));
      }
      if (auto color = parseColor(desired, "color")) {
        separator->setColor(*color);
      }
      if (const double* spacing = numProp(desired, "spacing")) {
        separator->setSpacing(scaled(*spacing));
      }
      if (const std::string* orientation = strProp(desired, "orientation")) {
        if (*orientation == "horizontal") {
          separator->setOrientation(SeparatorOrientation::HorizontalRule);
        } else if (*orientation == "vertical") {
          separator->setOrientation(SeparatorOrientation::VerticalRule);
        } else if (*orientation == "auto") {
          separator->setOrientation(SeparatorOrientation::Auto);
        } else {
          kLog.warn("ui tree: separator has unknown orientation '{}'", *orientation);
        }
      }
      return;
    }

    if (desired.type == "progress") {
      auto* progress = static_cast<ProgressBar*>(node);
      if (const double* value = numProp(desired, "progress")) {
        progress->setProgress(std::clamp(static_cast<float>(*value), 0.0f, 1.0f));
      }
      if (auto fill = parseColor(desired, "fill")) {
        progress->setFill(*fill);
      }
      if (auto track = parseColor(desired, "track")) {
        progress->setTrack(*track);
      }
      if (const double* radius = numProp(desired, "radius")) {
        progress->setRadius(scaled(*radius));
      }
      progress->setSize(
          width != nullptr ? scaled(*width) : node->width(), height != nullptr ? scaled(*height) : node->height()
      );
      return;
    }

    if (desired.type == "button") {
      auto* button = static_cast<Button*>(node);
      const std::string* text = strProp(desired, "text");
      const std::string* glyph = strProp(desired, "glyph");
      if (glyph != nullptr) {
        button->setGlyph(*glyph);
      }
      if (text != nullptr) {
        button->setText(*text);
      } else if (glyph != nullptr) {
        button->setText("");
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        button->setFontSize(scaled(*fontSize));
      } else if (text != nullptr && !text->empty()) {
        // Guarded on non-empty text: setFontSize creates the label, which
        // would flip a glyph-only button to the taller text chrome tier.
        button->setFontSize(Style::fontSizeBody * m_scale);
      }
      if (const double* glyphSize = numProp(desired, "glyphSize")) {
        button->setGlyphSize(scaled(*glyphSize));
      } else if (glyph != nullptr) {
        button->setGlyphSize(Style::fontSizeBody * m_scale);
      }
      if (auto variant = parseButtonVariant(desired)) {
        button->setVariant(*variant);
      }
      if (const std::string* contentAlign = strProp(desired, "contentAlign")) {
        if (*contentAlign == "start") {
          button->setContentAlign(ButtonContentAlign::Start);
        } else if (*contentAlign == "end") {
          button->setContentAlign(ButtonContentAlign::End);
        } else if (*contentAlign == "center") {
          button->setContentAlign(ButtonContentAlign::Center);
        } else {
          kLog.warn("ui tree: unknown button contentAlign '{}'", *contentAlign);
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        button->setEnabled(*enabled);
      }
      if (const bool* selected = boolProp(desired, "selected")) {
        button->setSelected(*selected);
      }
      // Unconditional: a tooltip dropped between renders must clear on the
      // retained Button. An empty text routes to InputArea::clearTooltip().
      const std::string* tooltip = strProp(desired, "tooltip");
      button->setTooltip(tooltip != nullptr ? *tooltip : "");
      // Like the wrapper controls, hover handlers clear on removal instead of
      // being retained: a stale one keeps firing and keeps the Button's
      // InputArea enabled for a callback the tree no longer declares.
      if (const std::string* onHover = callbackProp(desired, "onHover"); onHover != nullptr) {
        if (*onHover != slot.hoverCallbackName) {
          releaseHover(node);
          slot.hoverCallbackName = *onHover;
          button->setOnEnter([this, name = slot.hoverCallbackName, key = desired.key, node]() {
            openHover(name, key, node);
          });
          button->setOnLeave([this, node]() { releaseHover(node); });
        }
      } else if (!slot.hoverCallbackName.empty()) {
        releaseHover(node);
        slot.hoverCallbackName.clear();
        button->setOnEnter(nullptr);
        button->setOnLeave(nullptr);
      }
      if (const std::string* onClick = strProp(desired, "onClick");
          onClick != nullptr && *onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        button->setOnClick([this, name = slot.callbackName]() {
          if (m_sink) {
            m_sink(ControlCallback{name});
          }
        });
      }
      if (const std::string* onRightClick = strProp(desired, "onRightClick");
          onRightClick != nullptr && *onRightClick != slot.rightCallbackName) {
        slot.rightCallbackName = *onRightClick;
        button->setOnRightClick([this, name = slot.rightCallbackName]() {
          if (m_sink) {
            m_sink(ControlCallback{name});
          }
        });
      }
      // Compact hosts (bar widgets): drop the settings-tier control chrome
      // (min-height, wide padding) and hug the content — a bar capsule is
      // barely one control height tall. Applied after setText/setGlyph, whose
      // label creation re-applies the text-tier chrome. Explicit width/height
      // below still override.
      if (m_compactControls) {
        button->setMinHeight(0.0f);
        button->setPadding(Style::spaceXs * m_scale);
      }
      if (auto size = parseControlSize(desired)) {
        button->setControlHeight(scaled(*size));
      }
      if (width != nullptr) {
        button->setMinWidth(scaled(*width));
        button->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        button->setMinHeight(scaled(*height));
        button->setMaxHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "graph") {
      auto* graph = static_cast<Graph*>(node);
      if (const std::vector<double>* values = arrayProp(desired, "values")) {
        graph->setValues(toFloatSeries(*values));
      }
      if (const std::vector<double>* values2 = arrayProp(desired, "values2")) {
        graph->setValues2(toFloatSeries(*values2));
      }
      if (auto color = parseColor(desired, "color")) {
        graph->setColor(*color);
      }
      if (auto color2 = parseColor(desired, "color2")) {
        graph->setColor2(*color2);
      }
      if (const double* lineWidth = numProp(desired, "lineWidth")) {
        graph->setLineWidth(scaled(*lineWidth));
      }
      if (const double* fillOpacity = numProp(desired, "fillOpacity")) {
        graph->setFillOpacity(std::clamp(static_cast<float>(*fillOpacity), 0.0f, 1.0f));
      }
      graph->setSize(
          width != nullptr ? scaled(*width) : node->width(), height != nullptr ? scaled(*height) : node->height()
      );
      return;
    }

    if (desired.type == "toggle") {
      auto* toggle = static_cast<Toggle*>(node);
      if (const bool* checked = boolProp(desired, "checked")) {
        const double v = *checked ? 1.0 : 0.0;
        if (!slot.lastScalar.has_value() || *slot.lastScalar != v) {
          slot.lastScalar = v;
          toggle->setChecked(*checked);
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        toggle->setEnabled(*enabled);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        toggle->setOnChange([this, name = slot.callbackName](bool value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value ? "true" : "false"});
          }
        });
      }
      return;
    }

    if (desired.type == "slider") {
      auto* slider = static_cast<Slider*>(node);
      const double* minV = numProp(desired, "min");
      const double* maxV = numProp(desired, "max");
      if (minV != nullptr || maxV != nullptr) {
        slider->setRange(minV != nullptr ? *minV : slider->minValue(), maxV != nullptr ? *maxV : slider->maxValue());
      }
      if (const double* step = numProp(desired, "step")) {
        slider->setStep(*step);
      }
      if (const double* value = numProp(desired, "value")) {
        // Don't fight an in-progress drag, and only re-apply a changed value.
        if (!slider->dragging() && (!slot.lastScalar.has_value() || *slot.lastScalar != *value)) {
          slot.lastScalar = *value;
          slider->setValue(*value);
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        slider->setEnabled(*enabled);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        slider->setOnValueChanged([this, name = slot.callbackName](double value) {
          if (m_sink) {
            m_sink(ControlCallback{name, std::format("{}", value), "", true});
          }
        });
      }
      if (const std::string* onDragEnd = strProp(desired, "onDragEnd");
          onDragEnd != nullptr && *onDragEnd != slot.dragEndCallbackName) {
        slot.dragEndCallbackName = *onDragEnd;
        slider->setOnDragEnd([this, name = slot.dragEndCallbackName]() {
          if (m_sink) {
            m_sink(ControlCallback{name});
          }
        });
      }
      if (auto size = parseControlSize(desired)) {
        slider->setControlHeight(scaled(*size));
      }
      return;
    }

    if (desired.type == "select") {
      auto* select = static_cast<Select*>(node);
      const std::string* onChangeProp = strProp(desired, "onChange");
      if (const std::vector<std::string>* options = strArrayProp(desired, "options")) {
        select->setOptions(*options);
        slot.lastScalar.reset(); // re-apply the selection against the new option set
      }
      if (const double* index = numProp(desired, "selectedIndex")) {
        if (!slot.lastScalar.has_value() || *slot.lastScalar != *index) {
          slot.lastScalar = *index;
          select->setSelectedIndexSilently(static_cast<std::size_t>(std::max(0.0, *index)));
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        select->setEnabled(*enabled);
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        select->setPlaceholder(*placeholder);
      }
      if (onChangeProp != nullptr && *onChangeProp != slot.callbackName) {
        slot.callbackName = *onChangeProp;
        select->setOnSelectionChanged([this, name = slot.callbackName](std::size_t idx, std::string_view text) {
          if (m_sink) {
            m_sink(ControlCallback{name, std::format("{}", idx), std::string(text)});
          }
        });
      }
      if (width != nullptr) {
        const float w = scaled(*width);
        select->setMinWidth(w);
        if (const double* grow = numProp(desired, "flexGrow"); grow == nullptr || *grow <= 0.0) {
          select->setMaxWidth(w);
        }
      }
      if (auto size = parseControlSize(desired)) {
        select->setControlHeight(scaled(*size));
      }
      if (height != nullptr) {
        select->setControlHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "input") {
      auto* input = static_cast<Input*>(node);
      // Uncontrolled: the host owns the text buffer/cursor. `value` seeds the
      // field once at creation; later reconciles never overwrite what the user
      // typed (the node key keeps the same Input instance alive across renders).
      if (!slot.seeded) {
        if (const std::string* value = strProp(desired, "value")) {
          input->setValue(*value);
        }
        // `focus` requests keyboard focus once, when the control is created
        // (a fresh key makes a new control) — never on later re-renders.
        if (const bool* focus = boolProp(desired, "focus"); focus != nullptr && *focus && m_focusSink) {
          m_focusSink(input->inputArea());
        }
        slot.seeded = true;
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        input->setPlaceholder(*placeholder);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        input->setFontSize(scaled(*fontSize));
      }
      // Multiline before password: they are mutually exclusive and multiline wins.
      if (const bool* multiline = boolProp(desired, "multiline")) {
        input->setMultiline(*multiline);
      }
      if (const bool* password = boolProp(desired, "password")) {
        input->setPasswordMode(*password);
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        input->setEnabled(*enabled);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
        input->setOnChange([this, name = slot.callbackName](const std::string& value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value, "", true});
          }
        });
      }
      if (const std::string* onSubmit = strProp(desired, "onSubmit");
          onSubmit != nullptr && *onSubmit != slot.submitCallbackName) {
        slot.submitCallbackName = *onSubmit;
        input->setOnSubmit([this, name = slot.submitCallbackName](const std::string& value) {
          if (m_sink) {
            m_sink(ControlCallback{name, value});
          }
        });
      }
      if (width != nullptr) {
        input->setMinLayoutWidth(scaled(*width));
      }
      if (auto size = parseControlSize(desired)) {
        input->setControlHeight(scaled(*size));
      }
      if (height != nullptr) {
        input->setControlHeight(scaled(*height));
      }
      return;
    }

    if (desired.type == "scroll") {
      auto* scroll = static_cast<ScrollView*>(node);
      if (auto fill = parseColor(desired, "fill")) {
        scroll->setFill(*fill);
      }
      if (const double* radius = numProp(desired, "radius")) {
        scroll->setRadius(scaled(*radius));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        scroll->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : 1.0f);
      }
      // gap / padding / align / justify configure the inner content Flex that
      // hosts the children.
      Flex* content = scroll->content();
      if (const double* gap = numProp(desired, "gap")) {
        content->setGap(scaled(*gap));
      }
      if (auto align = parseAlign(desired)) {
        content->setAlign(*align);
      }
      if (auto justify = parseJustify(desired)) {
        content->setJustify(*justify);
      }
      const double* padding = numProp(desired, "padding");
      const double* paddingV = numProp(desired, "paddingV");
      const double* paddingH = numProp(desired, "paddingH");
      if (padding != nullptr || paddingV != nullptr || paddingH != nullptr) {
        const float fallback = padding != nullptr ? scaled(*padding) : 0.0f;
        content->setPadding(
            paddingV != nullptr ? scaled(*paddingV) : fallback, paddingH != nullptr ? scaled(*paddingH) : fallback
        );
      }
      if (width != nullptr) {
        scroll->setMinWidth(scaled(*width));
        scroll->setMaxWidth(scaled(*width));
      }
      if (height != nullptr) {
        scroll->setMinHeight(scaled(*height));
        scroll->setMaxHeight(scaled(*height));
      }
      return;
    }

    // spacer (and any future no-prop control): common props only.
    if (width != nullptr || height != nullptr) {
      node->setSize(
          width != nullptr ? scaled(*width) : node->width(), height != nullptr ? scaled(*height) : node->height()
      );
    }
  }

} // namespace ui
