#pragma once

#include "config/config_types.h"
#include "core/ui_phase.h"
#include "ipc/ipc_invocation_context.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_gesture.h"
#include "ui/palette.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

class AnimationManager;
class Box;
class InputArea;
class Label;
struct PointerEvent;

namespace noctalia::bar {
  class WidgetActionDispatcher;
}

// Which side of the value the glyph sits on (`glyph_position`). With peek on, the value reveals on the other side.
enum class GlyphPosition : std::uint8_t { Before, After };

class Widget {
public:
  // Whether a panel action opens the panel or toggles it.
  enum class PanelActivation : std::uint8_t { Toggle, Open };

  using UpdateCallback = std::function<void()>;
  using RedrawCallback = std::function<void()>;
  using FrameTickRequestCallback = std::function<void()>;
  using PanelToggleCallback = std::function<void(
      std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
      std::optional<float> anchorSurfaceY, PanelActivation activation
  )>;

  virtual ~Widget();

  virtual void create() = 0;
  void layout(Renderer& renderer, float containerWidth, float containerHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, containerWidth, containerHeight);
    layoutCustomLabel(renderer, containerWidth, containerHeight);
    applyCommonGlyphVisibility();
    syncOuterFromRoot();
  }
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
    syncOuterFromRoot();
  }
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }
  virtual bool needsFrameTick() const { return false; }
  // True when the widget renders a value that changes every second. The bar uses
  // this to decide whether it needs second-precision time ticks at all.
  virtual bool wantsSecondTicks() const { return false; }
  virtual bool onPointerEvent(const PointerEvent& event) {
    (void)event;
    return false;
  }

  virtual bool noGapAroundMe() const noexcept { return false; }
  // Takes a share of its lane's free space (start and end lanes only; the center lane sizes to its content).
  virtual bool expandsInLane() const noexcept { return false; }
  // Layout-only or non-interactive bar widgets: clicks pass through to bar dead-zone handlers.
  virtual bool isBarClickThrough() const noexcept { return m_nonInteractive; }
  // Widgets with interactive sub-items (workspaces, taskbar, tray) manage hover per item and
  // opt out of the bar's generic whole-widget hover highlight.
  virtual bool wantsBarHoverHighlight() const noexcept { return !m_nonInteractive; }
  // Whether the hover box tints while the widget is hovered. The box still sizes the hit target.
  virtual bool wantsBarHoverTint() const noexcept { return true; }

  // The node this widget built. Widgets size and position it in doLayout().
  Node* root() const noexcept { return m_innerRoot; }
  // The node the bar mounts: a transparent InputArea wrapping root(), carrying gesture actions.
  Node* outerNode() const noexcept { return m_outer ? m_outer.get() : m_outerPtr; }
  float width() const noexcept;
  float height() const noexcept;

  std::unique_ptr<Node> releaseRoot();

  // Merges the four binding layers and installs handlers on the gesture area. Re-runs on reload.
  void resolveGestureBindings(
      std::string_view widgetType, const WidgetConfig* widgetConfig,
      const noctalia::bar::WidgetActionBindings::ActionTable* barActions, std::string_view barContext,
      const noctalia::bar::WidgetActionDispatcher* dispatcher
  );
  void applyCommonOptions(
      const CommonWidgetOptions& options, FontWeight barFontWeight, const std::string& barFontFamily,
      std::string_view logContext
  );
  void setActionContext(IpcInvocationContext context) { m_actionContext = std::move(context); }
  const noctalia::bar::WidgetActionBindings& gestureBindings() const noexcept { return m_gestureBindings; }

  void setAnimationManager(AnimationManager* mgr) noexcept;
  void setUpdateCallback(UpdateCallback callback);
  void setRedrawCallback(RedrawCallback callback);
  void setFrameTickRequestCallback(FrameTickRequestCallback callback);
  void setPanelToggleCallback(PanelToggleCallback callback);
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  float contentScale() const noexcept { return m_contentScale; }
  void setFontScale(float scale) noexcept { m_fontScale = scale; }
  float fontScale() const noexcept { return m_contentScale * m_fontScale; }
  float fontScaleMultiplier() const noexcept { return m_fontScale; }
  void setLabelFontWeight(FontWeight fontWeight) noexcept { m_labelFontWeight = fontWeight; }
  FontWeight labelFontWeight() const noexcept { return m_labelFontWeight; }
  void setLabelFontFamily(std::string family) noexcept { m_labelFontFamily = std::move(family); }
  const std::string& labelFontFamily() const noexcept { return m_labelFontFamily; }
  void setConfigName(std::string name) { m_configName = std::move(name); }
  std::string_view configName() const noexcept { return m_configName; }
  void setAnchor(bool anchor) noexcept { m_anchor = anchor; }
  bool isAnchor() const noexcept { return m_anchor; }
  // Bar-owned gate: when false the bar never shows this widget's tooltips; widgets keep setting their content.
  bool showsTooltip() const noexcept { return m_showTooltip; }

  void setBarCapsuleSpec(WidgetBarCapsuleSpec spec) noexcept { m_barCapsuleSpec = std::move(spec); }
  void setWidgetForeground(std::optional<ColorSpec> color) noexcept { m_widgetForeground = color; }
  void setWidgetIconColor(std::optional<ColorSpec> color) noexcept { m_widgetIconColor = color; }
  void setNonInteractive(bool nonInteractive) noexcept;
  bool nonInteractive() const noexcept { return m_nonInteractive; }
  // Bar-owned: blocks all pointer input while the member is clipped out of a collapsed accordion.
  void setBarPointerSuppressed(bool suppressed) noexcept;
  bool barPointerSuppressed() const noexcept { return m_barPointerSuppressed; }
  const WidgetBarCapsuleSpec& barCapsuleSpec() const noexcept { return m_barCapsuleSpec; }
  void setBarCapsuleScene(Node* shell, Box* box) noexcept;
  Node* barCapsuleShell() const noexcept { return m_capsuleShell; }
  Box* barCapsuleBox() const noexcept { return m_capsuleBox; }
  void setBarHoverBox(Box* box) noexcept { m_hoverBox = box; }
  Box* barHoverBox() const noexcept { return m_hoverBox; }
  void setBarHoverProgress(float progress) noexcept { m_hoverProgress = progress; }
  float barHoverProgress() const noexcept { return m_hoverProgress; }
  bool peekEnabled() const noexcept { return m_peekEnabled; }
  float peekDelayMs() const noexcept { return m_peekDelayMs; }
  // Bar-owned peek state: whether the value is (being) revealed, and the eased 0..1 reveal progress.
  bool peekExpanded() const noexcept { return m_peekExpanded; }
  void setPeekExpanded(bool expanded) noexcept { m_peekExpanded = expanded; }
  float peekProgress() const noexcept { return m_peekProgress; }
  void setPeekProgress(float progress) noexcept { m_peekProgress = progress; }
  // Owner token for the bar's peek animation; ~Widget cancels it.
  const void* peekAnimationOwner() const noexcept { return &m_peekProgress; }
  // Outermost node for flex layout / anchor alignment (capsule shell when enabled).
  Node* layoutBoundsNode() const noexcept { return m_capsuleShell != nullptr ? m_capsuleShell : outerNode(); }
  float resolvedBarCapsuleRadius(float width, float height) const noexcept;

  // Whether the bar should paint the decorative capsule for this frame (spec enabled + visible ink).
  virtual bool shouldShowBarCapsule() const;

  // Resolved primary label color: `[widget.*] color` when set, else `capsule_foreground` when capsule styling is
  // enabled, else `fallback` (e.g. colorSpecFromRole(OnSurface)).
  ColorSpec widgetForegroundOr(const ColorSpec& fallback) const noexcept;
  // Resolved icon color: `[widget.*] icon_color` when set, else the foreground chain
  // (`color` → `capsule_foreground` → `fallback`), so a bare `color` still tints icons.
  ColorSpec widgetIconColorOr(const ColorSpec& fallback) const noexcept;

protected:
  void requestUpdate();
  void requestRedraw();
  void requestFrameTick();
  void requestPanelToggle(
      std::string_view panelId, std::string_view context = {}, std::optional<float> anchorSurfaceX = std::nullopt,
      std::optional<float> anchorSurfaceY = std::nullopt, PanelActivation activation = PanelActivation::Toggle
  );
  // Widgets that render the shared `custom_label` themselves (sysmon) opt out of the base
  // wrapper; their own options resolve the same `custom_label` key.
  void markCustomLabelHandled() noexcept { m_customLabelHandled = true; }
  void setRoot(std::unique_ptr<Node> root);
  void clearReleasedRoot() noexcept {
    m_outerPtr = nullptr;
    m_innerRoot = nullptr;
    m_gestureArea = nullptr;
  }
  // Runs the action bound to `gesture`, if any. Returns whether it was handled.
  bool dispatchGesture(noctalia::bar::Gesture gesture);
  // Called just before a bound action runs, so a widget can snapshot state for an optimistic
  // update. Match on `action` when the update only makes sense for one verb.
  virtual void onGestureDispatch(noctalia::bar::Gesture gesture, const noctalia::bar::WidgetAction& action) {
    (void)gesture;
    (void)action;
  }
  virtual void doLayout(Renderer& renderer, float containerWidth, float containerHeight) = 0;
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }
  // Opts the widget into peek: only the glyph shows until the widget is hovered.
  void setPeek(bool enabled, int delayMs) noexcept;

  struct GlyphValueLayout {
    Node* glyph = nullptr; // Glyph or Image, already measured; skipped when null or hidden
    Node* value = nullptr; // already measured; skipped when null or hidden
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool vertical = false;
    float horizontalGap = 0.0F; // applied only when both nodes are shown
    float verticalGap = 0.0F;
  };
  // Places glyph and value (direct children of root()) along the bar's main axis, sizes root(), applies peek.
  void layoutGlyphValue(const GlyphValueLayout& layout);
  // root() must already be sized to the full content laid out from 0 along the main axis. Clips root() to the
  // glyph plus the revealed share of the rest; returns the main-axis offset to add to every content position.
  float applyPeek(float contentMain, float glyphMain, GlyphPosition glyphPosition, bool vertical);

  float m_contentScale = 1.0F;
  float m_fontScale = 1.0F;
  FontWeight m_labelFontWeight = FontWeight::Medium;
  std::string m_labelFontFamily; // empty = inherit renderer-global family
  std::string m_customLabelText; // shared `custom_label` from the common widget options
  bool m_showGlyph = true;       // shared `show_glyph` from the common widget options
  std::string m_configName;
  bool m_anchor = false;
  bool m_showTooltip = true;
  AnimationManager* m_animations = nullptr;
  UpdateCallback m_updateCallback;
  RedrawCallback m_redrawCallback;
  FrameTickRequestCallback m_frameTickRequestCallback;
  PanelToggleCallback m_panelToggleCallback;
  WidgetBarCapsuleSpec m_barCapsuleSpec{};
  std::optional<ColorSpec> m_widgetForeground;
  std::optional<ColorSpec> m_widgetIconColor;
  bool m_nonInteractive = false;
  bool m_barPointerSuppressed = false;
  Node* m_capsuleShell = nullptr;
  Box* m_capsuleBox = nullptr;
  Box* m_hoverBox = nullptr;
  float m_hoverProgress = 0.0F;

private:
  void installGestureHandlers();
  bool bindingRepeatsEveryScrollStep(noctalia::bar::Gesture gesture) const;
  // An enabled InputArea captures hover (and the highlight) even with no accepted buttons, so the
  // wrapper stays inert until something is actually bound to it.
  void updateGestureAreaEnabled() noexcept;
  void syncOuterHitTestVisible() noexcept;
  // The wrapper carries no geometry of its own; it mirrors the laid-out content: the
  // custom-label wrapper when one is active, otherwise root(), which widgets size in
  // doLayout() and hide in doUpdate() (hide_when_no_media, hide_when_off, ...).
  void syncOuterFromRoot() noexcept;
  void layoutCustomLabel(Renderer& renderer, float containerWidth, float containerHeight);
  // With the shared `show_glyph` option off, hides the widget's icon glyphs wherever they sit
  // in the subtree. Only Glyph-class nodes are affected; image content (tray, taskbar, media
  // artwork) is not. Runs at setRoot() and after every layout; typed `show_glyph` handlers
  // (sysmon, keyboard_layout) also consume the key, and this walk applies on top, idempotently.
  void applyCommonGlyphVisibility();
  static void hideGlyphNodes(Node& node);

  // m_outer owns m_innerRoot as its only child until releaseRoot() hands it to the bar.
  std::unique_ptr<Node> m_outer;
  Node* m_outerPtr = nullptr;
  Node* m_innerRoot = nullptr;
  Node* m_customLabelWrapper = nullptr;
  Label* m_customLabel = nullptr;
  bool m_customLabelHandled = false;
  InputArea* m_gestureArea = nullptr;
  // The widget's own root area and what it claimed before any binding was applied.
  InputArea* m_innerArea = nullptr;
  std::uint32_t m_innerBaseButtons = 0;
  std::uint32_t m_innerBaseScrollDirections = 0;
  noctalia::bar::WidgetActionBindings m_gestureBindings;
  noctalia::bar::ScrollRepeatMode m_scrollRepeatMode = noctalia::bar::ScrollRepeatMode::Auto;
  const noctalia::bar::WidgetActionDispatcher* m_actionDispatcher = nullptr;
  IpcInvocationContext m_actionContext;
  bool m_peekEnabled = false;
  float m_peekDelayMs = 0.0F;
  bool m_peekExpanded = false;
  float m_peekProgress = 0.0F;
};
