#include "ui/ui_tree_reconciler.h"

#include "core/log.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
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
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/ui_tree.h"

#include <cmath>
#include <format>
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

    // Role token ("primary", "on_surface", …) or hex ("#rrggbb[aa]").
    std::optional<ColorSpec> parseColor(const UiTreeNode& node, const char* key) {
      const std::string* token = strProp(node, key);
      if (token == nullptr) {
        return std::nullopt;
      }
      if (auto role = colorRoleFromToken(*token); role.has_value()) {
        return colorSpecFromRole(*role);
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

    // The Node that a control's children reconcile into. Flex containers host
    // their children directly; a ScrollView hosts them in its inner content
    // Flex. Any other control returns nullptr — it cannot have children.
    Node* childContainer(const std::string& type, Node* node) {
      if (node == nullptr) {
        return nullptr;
      }
      if (type == "column" || type == "row") {
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

    InputArea* inputAreaFromSlot(Node* node) { return dynamic_cast<InputArea*>(node); }

    // box/image get an InputArea wrapper only when clickable (see createControl).
    // If a reconcile flips that need, the existing node can't be reused — its
    // structure no longer matches — so it must be rebuilt like a type change.
    bool clickableWrapMismatch(const UiTreeNode& want, Node* node) {
      if (want.type != "box" && want.type != "image") {
        return false;
      }
      const bool wantsWrapper = strProp(want, "onClick") != nullptr;
      const bool hasWrapper = inputAreaFromSlot(node) != nullptr;
      return wantsWrapper != hasWrapper;
    }

    std::unique_ptr<Node> wrapClickable(std::unique_ptr<Node> control) {
      auto inputArea = std::make_unique<InputArea>();
      inputArea->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
      inputArea->addChild(std::move(control));
      return inputArea;
    }

    // Known prop keys per control type — an unknown prop is a loud skip, not a
    // silent no-op, so typos in plugin code surface immediately.
    const std::unordered_set<std::string>& knownProps(const std::string& type) {
      static const std::unordered_set<std::string> kCommon = {"width", "height", "flexGrow", "opacity", "visible"};
      static const std::unordered_set<std::string> kFlex = {
          "width", "height",  "flexGrow", "opacity", "visible", "gap",         "padding",  "paddingH", "paddingV",
          "align", "justify", "fill",     "radius",  "border",  "borderWidth", "minWidth", "minHeight"
      };
      static const std::unordered_set<std::string> kBox = {"width",       "height",   "flexGrow", "opacity",
                                                           "visible",     "fill",     "radius",   "border",
                                                           "borderWidth", "softness", "onClick"};
      static const std::unordered_set<std::string> kLabel = {"width",      "height",   "flexGrow", "opacity",
                                                             "visible",    "text",     "fontSize", "color",
                                                             "fontWeight", "maxWidth", "maxLines", "textAlign"};
      static const std::unordered_set<std::string> kGlyph = {"width",   "height", "flexGrow", "opacity",
                                                             "visible", "name",   "size",     "color"};
      static const std::unordered_set<std::string> kImage = {"width",   "height",      "flexGrow", "opacity",
                                                             "visible", "path",        "radius",   "fit",
                                                             "border",  "borderWidth", "onClick"};
      static const std::unordered_set<std::string> kSeparator = {"width",   "height",  "flexGrow",
                                                                 "opacity", "visible", "thickness",
                                                                 "color",   "spacing", "orientation"};
      static const std::unordered_set<std::string> kProgress = {"width",    "height", "flexGrow", "opacity", "visible",
                                                                "progress", "fill",   "track",    "radius"};
      static const std::unordered_set<std::string> kButton = {"width",     "height",      "flexGrow", "opacity",
                                                              "visible",   "text",        "glyph",    "fontSize",
                                                              "glyphSize", "variant",     "enabled",  "selected",
                                                              "onClick",   "onRightClick"};
      static const std::unordered_set<std::string> kGraph = {"width",   "height",    "flexGrow",   "opacity",
                                                             "visible", "values",    "values2",    "color",
                                                             "color2",  "lineWidth", "fillOpacity"};
      static const std::unordered_set<std::string> kInput = {"width",   "height",   "flexGrow",    "opacity",
                                                             "visible", "value",    "placeholder", "fontSize",
                                                             "enabled", "password", "onChange",    "onSubmit"};
      static const std::unordered_set<std::string> kSelect = {"width",       "height",  "flexGrow",      "opacity",
                                                              "visible",     "options", "selectedIndex", "enabled",
                                                              "placeholder", "onChange"};
      static const std::unordered_set<std::string> kSlider = {"width",   "height",  "flexGrow", "opacity",
                                                              "visible", "min",     "max",      "step",
                                                              "value",   "enabled", "onChange", "onDragEnd"};
      static const std::unordered_set<std::string> kToggle = {"width",   "height",  "flexGrow", "opacity",
                                                              "visible", "checked", "enabled",  "onChange"};
      static const std::unordered_set<std::string> kScroll = {"width",       "height", "flexGrow", "opacity",
                                                              "visible",     "fill",   "radius",   "border",
                                                              "borderWidth", "gap",    "padding",  "paddingH",
                                                              "paddingV",    "align",  "justify"};

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
      return kCommon;
    }

  } // namespace

  struct UiTreeReconciler::Slot {
    std::string type;
    std::string key;
    Node* node = nullptr;
    std::string callbackName;        // last-wired button onClick / control onChange target
    std::string rightCallbackName;   // last-wired button onRightClick target
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

  UiTreeReconciler::UiTreeReconciler() = default;
  UiTreeReconciler::~UiTreeReconciler() = default;

  bool UiTreeReconciler::reconcile(Flex& host, const UiTreeNode& tree, Renderer& renderer) {
    std::vector<UiTreeNode> desired;
    desired.push_back(tree); // single root child of the host container
    return syncChildren(host, m_rootSlots, desired, renderer);
  }

  void UiTreeReconciler::reset() { m_rootSlots.clear(); }

  std::unique_ptr<Node> UiTreeReconciler::createControl(const UiTreeNode& desired) {
    // ui.* flex containers default to stretching children across the cross axis
    // (like CSS flexbox), so a column fills its width without every plugin having
    // to say so. Override per node with `align`. (Flex's own C++ default is
    // Center, which the native shell relies on — only the ui.* layer changes.)
    if (desired.type == "column") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Vertical);
      flex->setAlign(FlexAlign::Stretch);
      return flex;
    }
    if (desired.type == "row") {
      auto flex = std::make_unique<Flex>();
      flex->setDirection(FlexDirection::Horizontal);
      flex->setAlign(FlexAlign::Stretch);
      return flex;
    }
    if (desired.type == "box") {
      if (strProp(desired, "onClick") != nullptr) {
        return wrapClickable(std::make_unique<Box>());
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
      if (strProp(desired, "onClick") != nullptr) {
        return wrapClickable(std::make_unique<Image>());
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
            || clickableWrapMismatch(desired[i], slots[i].node)) {
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
      node->setVisible(*visible);
    }
    if (const double* opacity = numProp(desired, "opacity")) {
      node->setOpacity(std::clamp(static_cast<float>(*opacity), 0.0f, 1.0f));
    }
    if (const double* grow = numProp(desired, "flexGrow")) {
      node->setFlexGrow(static_cast<float>(*grow));
    }

    const double* width = numProp(desired, "width");
    const double* height = numProp(desired, "height");

    if (desired.type == "column" || desired.type == "row") {
      auto* flex = static_cast<Flex*>(node);
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
        flex->setFill(*fill);
      }
      if (const double* radius = numProp(desired, "radius")) {
        flex->setRadius(scaled(*radius));
      }
      if (auto border = parseColor(desired, "border")) {
        const double* borderWidth = numProp(desired, "borderWidth");
        flex->setBorder(*border, borderWidth != nullptr ? scaled(*borderWidth) : 1.0f);
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
        flex->setMinHeight(scaled(*height));
        flex->setMaxHeight(scaled(*height));
      }
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
      if (const std::string* onClick = strProp(desired, "onClick");
          onClick != nullptr && *onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        if (auto* inputArea = inputAreaFromSlot(node)) {
          inputArea->setOnClick([this, name = slot.callbackName](const InputArea::PointerData&) {
            if (m_sink) {
              m_sink(ControlCallback{name});
            }
          });
        }
      }
      return;
    }

    if (desired.type == "label") {
      auto* label = static_cast<Label*>(node);
      if (const std::string* text = strProp(desired, "text")) {
        label->setText(*text);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        label->setFontSize(scaled(*fontSize));
      }
      if (auto color = parseColor(desired, "color")) {
        label->setColor(*color);
      }
      if (auto weight = parseFontWeight(desired)) {
        label->setFontWeight(*weight);
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
      if (const std::string* onClick = strProp(desired, "onClick");
          onClick != nullptr && *onClick != slot.callbackName) {
        slot.callbackName = *onClick;
        if (auto* inputArea = inputAreaFromSlot(node)) {
          inputArea->setOnClick([this, name = slot.callbackName](const InputArea::PointerData&) {
            if (m_sink) {
              m_sink(ControlCallback{name});
            }
          });
        }
      }
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
      }
      if (const double* glyphSize = numProp(desired, "glyphSize")) {
        button->setGlyphSize(scaled(*glyphSize));
      }
      if (auto variant = parseButtonVariant(desired)) {
        button->setVariant(*variant);
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        button->setEnabled(*enabled);
      }
      if (const bool* selected = boolProp(desired, "selected")) {
        button->setSelected(*selected);
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
      return;
    }

    if (desired.type == "select") {
      auto* select = static_cast<Select*>(node);
      if (const std::vector<std::string>* options = strArrayProp(desired, "options")) {
        select->setOptions(*options);
        slot.lastScalar.reset(); // re-apply the selection against the new option set
      }
      if (const double* index = numProp(desired, "selectedIndex")) {
        if (!slot.lastScalar.has_value() || *slot.lastScalar != *index) {
          slot.lastScalar = *index;
          select->setSelectedIndex(static_cast<std::size_t>(std::max(0.0, *index)));
        }
      }
      if (const bool* enabled = boolProp(desired, "enabled")) {
        select->setEnabled(*enabled);
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        select->setPlaceholder(*placeholder);
      }
      if (const std::string* onChange = strProp(desired, "onChange");
          onChange != nullptr && *onChange != slot.callbackName) {
        slot.callbackName = *onChange;
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
        slot.seeded = true;
      }
      if (const std::string* placeholder = strProp(desired, "placeholder")) {
        input->setPlaceholder(*placeholder);
      }
      if (const double* fontSize = numProp(desired, "fontSize")) {
        input->setFontSize(scaled(*fontSize));
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
