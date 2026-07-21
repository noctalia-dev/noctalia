#pragma once

#include "config/config_types.h"
#include "core/ui_phase.h"
#include "render/scene/node.h"
#include "wayland/layer_surface.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class AnimationManager;
class InputArea;
class Renderer;

class Panel {
public:
  virtual ~Panel() = default;

  virtual void create() = 0;
  void layout(Renderer& renderer, float width, float height) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, width, height);
  }
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
  }
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }
  virtual void onOpen(std::string_view context) { (void)context; }
  virtual void onClose() {}
  virtual void onIconThemeChanged() {}
  virtual void scrollFocusedInputIntoView(InputArea* area) { (void)area; }
  [[nodiscard]] virtual bool isContextActive(std::string_view context) const {
    (void)context;
    return false;
  }
  [[nodiscard]] virtual bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
    (void)sym;
    (void)modifiers;
    (void)pressed;
    (void)preedit;
    return false;
  }
  [[nodiscard]] virtual bool dismissTransientUi() { return false; }
  [[nodiscard]] virtual bool deferExternalRefresh() const { return false; }
  [[nodiscard]] virtual bool deferPointerRelayout() const { return false; }

  [[nodiscard]] virtual float preferredWidth() const = 0;
  [[nodiscard]] virtual float preferredHeight() const = 0;
  // Span the output's available extent on this axis (floating panels only). The
  // surface is dual-anchored with a requested size of 0 so the compositor
  // assigns the size, subtracting every exclusive zone on the output; the
  // preferred size is then only the fallback if no size is ever assigned.
  [[nodiscard]] virtual bool fillsWidth() const noexcept { return false; }
  [[nodiscard]] virtual bool fillsHeight() const noexcept { return false; }
  [[nodiscard]] virtual bool hasDecoration() const { return true; }
  [[nodiscard]] virtual LayerShellLayer layer() const { return LayerShellLayer::Top; }
  [[nodiscard]] virtual LayerShellKeyboard keyboardMode() const { return LayerShellKeyboard::OnDemand; }
  [[nodiscard]] virtual InputArea* initialFocusArea() const { return nullptr; }
  // Dynamic focus: consumed (returned once, then cleared) by PanelManager after
  // each update/layout pass. Lets content that arrives or changes after the
  // scene build request keyboard focus (e.g. a plugin input with focus = true).
  [[nodiscard]] virtual InputArea* takePendingFocusArea() { return nullptr; }
  // Panel placement policy. `Attached` merges with the bar when a suitable host
  // exists, `Floating` opens detached near the bar, and `Centered` opens in the
  // middle of the target output.
  [[nodiscard]] virtual PanelPlacement panelPlacement() const noexcept { return PanelPlacement::Floating; }
  // Floating screen position (one of kPanelPositions). Plugin panels override; built-in
  // panels resolve through shell.panel.*_position in PanelManager.
  [[nodiscard]] virtual std::string panelScreenPosition() const { return "auto"; }
  [[nodiscard]] virtual bool panelOpenNearClick() const { return false; }
  // For attached panels: which bar edge to attach to when more than one bar exists on
  // the target output. Returned value must outlive the call (use a string literal).
  [[nodiscard]] virtual std::string_view preferredAttachedBarPosition() const noexcept { return "top"; }
  // For attached panels: when true (default), the panel's background opacity tracks the
  // host bar's `backgroundOpacity` config and updates live. When false, the panel uses
  // attachedBackgroundOpacityOverride() instead — useful for panels whose contents need
  // a fixed alpha for legibility (e.g. wallpaper thumbnails).
  [[nodiscard]] virtual bool inheritsBarBackgroundOpacity() const noexcept { return true; }
  [[nodiscard]] virtual float attachedBackgroundOpacityOverride() const noexcept { return 1.0f; }
  [[nodiscard]] virtual bool wantsCloseAnimation() const noexcept { return true; }
  [[nodiscard]] virtual bool dismissOnOutsideClick() const { return true; }

  [[nodiscard]] Node* root() const noexcept { return m_root ? m_root.get() : m_rootPtr; }
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  [[nodiscard]] bool panelBordersEnabled() const noexcept { return m_panelBordersEnabled; }

  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPendingOpenContext(std::string_view context) { m_pendingOpenContext = std::string(context); }
  [[nodiscard]] std::string_view pendingOpenContext() const noexcept { return m_pendingOpenContext; }
  void setPanelBordersEnabled(bool enabled) noexcept {
    if (m_panelBordersEnabled == enabled) {
      return;
    }
    m_panelBordersEnabled = enabled;
    onPanelBordersChanged(enabled);
  }
  void setPanelCardOpacity(float opacity) noexcept {
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    if (m_panelCardOpacity == clamped) {
      return;
    }
    m_panelCardOpacity = clamped;
    onPanelCardOpacityChanged(clamped);
  }

  std::unique_ptr<Node> releaseRoot() {
    m_rootPtr = m_root.get();
    return std::move(m_root);
  }

  virtual void setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

protected:
  [[nodiscard]] float scaled(float value) const noexcept { return value * m_contentScale; }
  void setRoot(std::unique_ptr<Node> root) { m_root = std::move(root); }
  void clearReleasedRoot() noexcept { m_rootPtr = nullptr; }
  virtual void onPanelCardOpacityChanged(float opacity) { (void)opacity; }
  virtual void onPanelBordersChanged(bool enabled) { (void)enabled; }
  virtual void doLayout(Renderer& renderer, float width, float height) = 0;
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

  float m_contentScale = 1.0f;
  float m_panelCardOpacity = 1.0f;
  bool m_panelBordersEnabled = true;
  std::string m_pendingOpenContext;
  AnimationManager* m_animations = nullptr;

private:
  std::unique_ptr<Node> m_root;
  Node* m_rootPtr = nullptr;
};
