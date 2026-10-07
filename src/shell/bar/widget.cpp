#include "shell/bar/widget.h"

#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <format>

namespace {

  constexpr float kCapsuleInkEpsilon = 0.5F;
  constexpr Logger kLog("bar.actions");

} // namespace

Widget::~Widget() {
  if (m_animations != nullptr) {
    m_animations->cancelForOwner(this);
  }
}

ColorSpec Widget::widgetForegroundOr(const ColorSpec& fallback) const noexcept {
  // Per-widget `color` must win over bar/widget `capsule_foreground`, otherwise a bar-level
  // capsule_foreground (e.g. on_primary) overrides explicit `color = primary` after layout.
  if (m_widgetForeground.has_value()) {
    return *m_widgetForeground;
  }
  const auto& spec = m_barCapsuleSpec;
  if (spec.enabled && spec.foreground.has_value()) {
    return *spec.foreground;
  }
  return fallback;
}

ColorSpec Widget::widgetIconColorOr(const ColorSpec& fallback) const noexcept {
  // `icon_color` overrides; otherwise the icon inherits the full foreground chain
  // (`color` → `capsule_foreground` → fallback), so a bare `color` still tints icons.
  if (m_widgetIconColor.has_value()) {
    return *m_widgetIconColor;
  }
  return widgetForegroundOr(fallback);
}

bool Widget::shouldShowBarCapsule() const {
  if (!m_barCapsuleSpec.enabled) {
    return false;
  }
  const Node* r = root();
  if (r == nullptr || !r->visible()) {
    return false;
  }
  if (r->width() <= kCapsuleInkEpsilon || r->height() <= kCapsuleInkEpsilon) {
    return false;
  }
  // No scene children ⇒ nothing to frame (spacer bare node, empty tray flex, etc.).
  if (r->children().empty()) {
    return false;
  }
  return true;
}

float Widget::resolvedBarCapsuleRadius(float width, float height) const noexcept {
  const float maxRadius = std::max(0.0F, std::min(width, height) * 0.5F);
  if (!m_barCapsuleSpec.radius.has_value()) {
    return maxRadius;
  }
  return std::clamp(*m_barCapsuleSpec.radius * m_contentScale, 0.0F, maxRadius);
}

void Widget::setBarCapsuleScene(Node* shell, Box* box) noexcept {
  m_capsuleShell = shell;
  m_capsuleBox = box;
}

void Widget::setNonInteractive(bool nonInteractive) noexcept {
  m_nonInteractive = nonInteractive;
  syncOuterHitTestVisible();
  updateGestureAreaEnabled();
}

void Widget::setBarPointerSuppressed(bool suppressed) noexcept {
  if (m_barPointerSuppressed == suppressed) {
    return;
  }
  m_barPointerSuppressed = suppressed;
  syncOuterHitTestVisible();
}

void Widget::syncOuterHitTestVisible() noexcept {
  if (Node* outer = outerNode(); outer != nullptr) {
    outer->setHitTestVisible(!m_nonInteractive && !m_barPointerSuppressed);
  }
}

void Widget::updateGestureAreaEnabled() noexcept {
  if (m_gestureArea != nullptr) {
    m_gestureArea->setEnabled(!m_nonInteractive && !m_gestureBindings.empty());
  }
}

float Widget::width() const noexcept { return outerNode() != nullptr ? outerNode()->width() : 0.0F; }

float Widget::height() const noexcept { return outerNode() != nullptr ? outerNode()->height() : 0.0F; }

std::unique_ptr<Node> Widget::releaseRoot() {
  m_outerPtr = m_outer.get();
  return std::move(m_outer);
}

void Widget::setRoot(std::unique_ptr<Node> root) {
  m_innerRoot = root.get();
  m_innerArea = dynamic_cast<InputArea*>(m_innerRoot);
  m_innerBaseButtons = m_innerArea != nullptr ? m_innerArea->acceptedButtons() : 0;
  m_innerBaseScrollDirections = m_innerArea != nullptr ? m_innerArea->acceptedScrollDirections() : 0;

  auto gestureArea = ui::inputArea({});
  m_gestureArea = gestureArea.get();
  // Nothing is bound until resolveGestureBindings() runs, and an area with no accepted buttons
  // never wins the dispatcher's ancestor walk.
  m_gestureArea->setAcceptedButtons(0);
  const std::string customLabel = StringUtils::trim(m_customLabelText);
  if (root != nullptr && !customLabel.empty() && !m_customLabelHandled) {
    // Shared bar-widget label: wrap the widget root so the label adds to the widget's
    // laid-out footprint without every widget implementing it. Widgets that render the
    // label themselves (sysmon) call markCustomLabelHandled() and keep this path dormant.
    const bool innerVisible = root->visible();
    const bool innerParticipates = root->participatesInLayout();
    auto wrapper = ui::node({});
    m_customLabelWrapper = wrapper.get();
    auto label = ui::label({
        .out = &m_customLabel,
        .text = customLabel,
        .fontSize = Style::fontSizeBody * fontScale(),
        .fontWeight = m_labelFontWeight,
        .fontFamily = m_labelFontFamily,
        .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
    });
    wrapper->addChild(std::move(label));
    wrapper->addChild(std::move(root));
    wrapper->setVisible(innerVisible);
    wrapper->setParticipatesInLayout(innerParticipates);
    m_gestureArea->addChild(std::move(wrapper));
  } else if (root != nullptr) {
    m_gestureArea->addChild(std::move(root));
  }

  m_outer = std::move(gestureArea);
  m_outer->setHitTestVisible(!m_nonInteractive && !m_barPointerSuppressed);
  // Suppress before the first doLayout: widgets size their root there, so the first frame must
  // already measure collapsed glyphs instead of rendering one frame with full-size gaps.
  applyCommonGlyphVisibility();
  // Bindings are resolved before create() runs, so install them here too: whichever of the two
  // happens last is the one that wires the area up.
  installGestureHandlers();
  syncOuterFromRoot();
}

void Widget::syncOuterFromRoot() noexcept {
  Node* outer = outerNode();
  // The custom-label wrapper, when active, is the widget's laid-out footprint.
  Node* source = m_customLabelWrapper != nullptr ? m_customLabelWrapper : m_innerRoot;
  if (outer == nullptr || source == nullptr) {
    return;
  }
  outer->setSize(source->width(), source->height());
  outer->setVisible(source->visible());
  outer->setParticipatesInLayout(source->participatesInLayout());
}

void Widget::layoutCustomLabel(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_customLabelWrapper == nullptr || m_customLabel == nullptr || m_innerRoot == nullptr) {
    return;
  }

  const bool isVerticalBar = containerHeight > containerWidth;
  m_customLabel->setFontSize((isVerticalBar ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
  m_customLabel->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_customLabel->measure(renderer);

  const float labelWidth = m_customLabel->width();
  const float labelHeight = m_customLabel->height();
  const float contentWidth = m_innerRoot->width();
  const float contentHeight = m_innerRoot->height();
  // The gap only separates label from content; with empty content it would trail the label.
  const float gap = (isVerticalBar ? contentHeight : contentWidth) > 0.0F ? Style::spaceXs * m_contentScale : 0.0F;

  if (isVerticalBar) {
    const float width = std::max(labelWidth, contentWidth);
    m_customLabel->setPosition((width - labelWidth) * 0.5F, 0.0F);
    m_innerRoot->setPosition((width - contentWidth) * 0.5F, labelHeight + gap);
    m_customLabelWrapper->setSize(width, labelHeight + gap + contentHeight);
  } else {
    const float height = std::max(labelHeight, contentHeight);
    m_customLabel->setPosition(0.0F, (height - labelHeight) * 0.5F);
    m_innerRoot->setPosition(labelWidth + gap, (height - contentHeight) * 0.5F);
    m_customLabelWrapper->setSize(labelWidth + gap + contentWidth, height);
  }

  m_customLabelWrapper->setVisible(m_innerRoot->visible());
  m_customLabelWrapper->setParticipatesInLayout(m_innerRoot->participatesInLayout());
}

void Widget::hideGlyphNodes(Node& node) {
  for (const auto& child : node.children()) {
    if (auto* glyph = dynamic_cast<Glyph*>(child.get()); glyph != nullptr) {
      glyph->suppress();
    }
    hideGlyphNodes(*child);
  }
}

void Widget::applyCommonGlyphVisibility() {
  if (m_showGlyph || m_innerRoot == nullptr) {
    return;
  }
  hideGlyphNodes(*m_innerRoot);
}

void Widget::setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

void Widget::setUpdateCallback(UpdateCallback callback) { m_updateCallback = std::move(callback); }

void Widget::setRedrawCallback(RedrawCallback callback) { m_redrawCallback = std::move(callback); }

void Widget::setFrameTickRequestCallback(FrameTickRequestCallback callback) {
  m_frameTickRequestCallback = std::move(callback);
}

void Widget::setPanelToggleCallback(PanelToggleCallback callback) { m_panelToggleCallback = std::move(callback); }

void Widget::requestUpdate() {
  if (m_updateCallback) {
    m_updateCallback();
  }
}

void Widget::requestRedraw() {
  if (m_redrawCallback) {
    m_redrawCallback();
  }
}

void Widget::requestFrameTick() {
  if (m_frameTickRequestCallback) {
    m_frameTickRequestCallback();
  }
}

void Widget::requestPanelToggle(
    std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
    std::optional<float> anchorSurfaceY, PanelActivation activation
) {
  if (m_panelToggleCallback) {
    m_panelToggleCallback(panelId, context, anchorSurfaceX, anchorSurfaceY, activation);
  }
}

void Widget::applyCommonOptions(
    const CommonWidgetOptions& options, FontWeight barFontWeight, const std::string& barFontFamily,
    std::string_view logContext
) {
  setAnchor(options.anchor);
  setNonInteractive(!options.interactive);
  m_labelFontWeight =
      options.labelFontWeight.has_value() ? static_cast<FontWeight>(*options.labelFontWeight) : barFontWeight;
  std::string labelFontFamily = StringUtils::trim(options.labelFontFamily);
  if (labelFontFamily.empty()) {
    m_labelFontFamily = barFontFamily;
  } else {
    m_labelFontFamily = std::move(labelFontFamily);
  }

  m_fontScale = options.fontScale;
  m_customLabelText = options.customLabel;
  m_showGlyph = options.showGlyph;

  m_scrollRepeatMode = noctalia::bar::ScrollRepeatMode::Auto;
  if (const auto mode = noctalia::bar::parseScrollRepeatMode(options.scrollRepeat); mode.has_value()) {
    m_scrollRepeatMode = *mode;
  } else {
    kLog.error("{}.scroll_repeat: unknown mode \"{}\"", logContext, options.scrollRepeat);
  }
}

void Widget::resolveGestureBindings(
    std::string_view widgetType, const WidgetConfig* widgetConfig,
    const noctalia::bar::WidgetActionBindings::ActionTable* barActions, std::string_view barContext,
    const noctalia::bar::WidgetActionDispatcher* dispatcher
) {
  m_actionDispatcher = dispatcher;

  const std::string widgetContext = std::format("widget.{}", m_configName);
  // Named, not inlined: the span in Inputs borrows from it.
  const auto typeDefaults = noctalia::bar::gestureDefaultsForType(widgetType, widgetConfig);
  m_gestureBindings.resolve(
      noctalia::bar::WidgetActionBindings::Inputs{
          .builtinDefaults = noctalia::bar::builtinGestureDefaults(),
          .widgetDefaults = typeDefaults,
          .barActions = barActions,
          .widgetActions = noctalia::bar::findActionTable(widgetConfig),
          .reserved = noctalia::bar::reservedGesturesForType(widgetType),
          .widgetContext = widgetContext,
          .barContext = barContext,
          .widgetName = m_configName,
          .widgetType = widgetType,
      }
  );

  installGestureHandlers();
}

void Widget::installGestureHandlers() {
  if (m_gestureArea == nullptr) {
    return;
  }

  const auto bound = m_gestureBindings.boundGestures();

  std::uint32_t mask = 0;
  std::uint32_t scrollMask = 0;
  for (const auto gesture : noctalia::bar::allGestures()) {
    if (!bound.contains(gesture)) {
      continue;
    }
    for (const auto button : noctalia::bar::buttonsForGesture(gesture)) {
      mask |= InputArea::buttonMask(button);
    }
    if (const auto direction = noctalia::bar::scrollDirectionForGesture(gesture); direction.has_value()) {
      scrollMask |= InputArea::scrollDirectionMask(*direction);
    }
  }
  m_gestureArea->setAcceptedButtons(mask);
  updateGestureAreaEnabled();

  if (m_innerArea != nullptr) {
    // Both dispatcher walks start at the innermost area, so the widget's own root has to give up
    // whatever the wrapper is bound to. This is what lets a config binding override a gesture the
    // widget still handles itself, for scroll as much as for clicks.
    m_innerArea->setAcceptedButtons(m_innerBaseButtons & ~mask);
    m_innerArea->setAcceptedScrollDirections(m_innerBaseScrollDirections & ~scrollMask);
    // Hover resolves to the innermost area, so it carries the pointer cursor for the wrapper.
    if (mask != 0 && m_innerArea->cursorShape() == 0) {
      m_innerArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    }
  }

  // Runs once per widget per reload; the resolved set is the first thing to check when a binding
  // does not fire.
  std::string summary;
  for (const auto gesture : noctalia::bar::allGestures()) {
    const auto* action = m_gestureBindings.find(gesture);
    if (action == nullptr) {
      continue;
    }
    if (!summary.empty()) {
      summary += ", ";
    }
    summary += std::format(
        "{}={}", gestureConfigKey(gesture),
        action->kind == noctalia::bar::WidgetAction::Kind::Exec ? std::format("exec {}", action->args)
                                                                : action->commandLine()
    );
  }
  kLog.debug("widget.{}: {}", m_configName, summary.empty() ? "no gesture bindings" : summary);

  if (mask != 0) {
    m_gestureArea->setOnClick([this](const InputArea::PointerData& data) {
      if (const auto gesture = noctalia::bar::gestureForButton(data.button)) {
        dispatchGesture(*gesture);
      }
    });
  }

  m_gestureArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    const auto gesture = noctalia::bar::gestureForScroll(data.axis, data.scrollSteps());
    if (!gesture.has_value()) {
      // Report a scroll gesture we own as consumed even between detents, so a partly accumulated
      // flick does not leak to an ancestor mid-gesture.
      return m_gestureBindings.find(noctalia::bar::Gesture::ScrollUp) != nullptr
          || m_gestureBindings.find(noctalia::bar::Gesture::ScrollDown) != nullptr;
    }
    if (!data.scrollStepStartsGesture() && !bindingRepeatsEveryScrollStep(*gesture)) {
      return true;
    }
    return dispatchGesture(*gesture);
  });
}

bool Widget::bindingRepeatsEveryScrollStep(noctalia::bar::Gesture gesture) const {
  const auto* action = m_gestureBindings.find(gesture);
  const bool actionCycles = action != nullptr && m_actionDispatcher != nullptr && m_actionDispatcher->cycles(*action);
  return noctalia::bar::scrollRepeatsEveryStep(m_scrollRepeatMode, actionCycles);
}

bool Widget::dispatchGesture(noctalia::bar::Gesture gesture) {
  const auto* action = m_gestureBindings.find(gesture);
  if (action == nullptr) {
    return false;
  }

  onGestureDispatch(gesture, *action);

  // Panel actions re-enter through the bar's panel callback so the panel anchors at this widget
  // rather than at the compositor's focused output.
  if (action->kind == noctalia::bar::WidgetAction::Kind::Ipc && noctalia::bar::isAnchoredPanelVerb(action->verb)) {
    const auto args = noctalia::bar::parsePanelVerbArgs(action->args);
    if (args.panelId.empty()) {
      kLog.error(
          "widget.{}.actions.{}: \"{}\" needs a panel id", m_configName, gestureConfigKey(gesture), action->verb
      );
      return false;
    }
    const auto activation =
        action->verb == "panel-open" ? Widget::PanelActivation::Open : Widget::PanelActivation::Toggle;
    requestPanelToggle(args.panelId, args.panelContext, std::nullopt, std::nullopt, activation);
    return true;
  }

  if (m_actionDispatcher == nullptr) {
    return false;
  }

  // The dispatcher reports the command and the failure itself.
  return m_actionDispatcher->run(*action, m_actionContext);
}
