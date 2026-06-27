#pragma once

#include "core/ui_phase.h"

#include <algorithm>
#include <memory>
#include <string>

class Flex;
class Label;
class Renderer;

namespace control_center {

  void applySectionCardStyle(Flex& card, float scale = 1.0f, float fillOpacity = 1.0f, bool showBorder = true);
  Label* addTitle(Flex& parent, const std::string& text, float scale = 1.0f);
  void addBody(Flex& parent, const std::string& text, float scale = 1.0f);

  // Card header row: a card-title-styled label (flexGrow) ready for trailing
  // controls appended by the caller. Used for titled group cards.
  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale = 1.0f);

} // namespace control_center

class Tab {
public:
  virtual ~Tab() = default;

  // Creates and returns the tab's root Flex container.
  // The returned node is owned by the caller (added to m_tabBodies).
  // Implementations may cache a raw pointer to it for later use.
  virtual std::unique_ptr<Flex> create() = 0;

  // Optional trailing header actions shown in the shared control-center header
  // while this tab is active.
  virtual std::unique_ptr<Flex> createHeaderActions();

  // Called by ControlCenterPanel::layout() with the available content dimensions.
  void layout(Renderer& renderer, float contentWidth, float bodyHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, contentWidth, bodyHeight);
  }

  // Called by ControlCenterPanel::update() every frame.
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
  }

  // Called every Wayland frame callback with elapsed milliseconds. Default is a no-op.
  // Tabs override this to advance per-frame animations independently of data arrival.
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }

  // Called when the tab becomes visible or hidden.
  virtual void setActive(bool active) { (void)active; }

  // Called when the panel closes. Null out all raw pointers to freed nodes.
  virtual void onClose() {}

  // Allows a tab to close transient UI (menus/popovers) when external empty
  // chrome areas are clicked.
  virtual bool dismissTransientUi() { return false; }

  void setContentScale(float scale) noexcept { m_contentScale = scale; }
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

protected:
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  [[nodiscard]] bool panelBordersEnabled() const noexcept { return m_panelBordersEnabled; }
  [[nodiscard]] float scaled(float value) const noexcept { return value * m_contentScale; }
  virtual void onPanelCardOpacityChanged(float opacity) { (void)opacity; }
  virtual void onPanelBordersChanged(bool enabled) { (void)enabled; }
  virtual void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
    (void)renderer;
    (void)contentWidth;
    (void)bodyHeight;
  }
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

private:
  float m_contentScale = 1.0f;
  float m_panelCardOpacity = 1.0f;
  bool m_panelBordersEnabled = true;
};
