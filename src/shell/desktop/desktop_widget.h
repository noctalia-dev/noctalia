#pragma once

#include "config/config_types.h"
#include "core/ui_phase.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class AnimationManager;
class Box;
class Renderer;

class DesktopWidget {
public:
  using UpdateCallback = std::function<void()>;
  using LayoutCallback = std::function<void()>;
  using RedrawCallback = std::function<void()>;
  using FrameTickRequestCallback = std::function<void()>;

  virtual ~DesktopWidget() = default;

  virtual void create() = 0;

  void layout(Renderer& renderer);
  void update(Renderer& renderer);

  [[nodiscard]] virtual bool wantsSecondTicks() const { return false; }
  [[nodiscard]] virtual bool needsFrameTick() const { return false; }
  virtual void onFrameTick(float deltaMs, Renderer& renderer) {
    (void)deltaMs;
    (void)renderer;
  }

  [[nodiscard]] Node* root() const noexcept { return m_contentRoot; }
  [[nodiscard]] float intrinsicWidth() const noexcept;
  [[nodiscard]] float intrinsicHeight() const noexcept;
  std::unique_ptr<Node> releaseRoot();

  void setAnimationManager(AnimationManager* manager) noexcept { m_animations = manager; }
  // Content updates must only mutate existing scene nodes. They are handled
  // in-place by the desktop widget hosts and must not assume a relayout or
  // scene rebuild.
  void setUpdateCallback(UpdateCallback callback) { m_updateCallback = std::move(callback); }
  // Use this when a widget's intrinsic size or node geometry changed and the
  // host must rerun update+layout on the widget.
  void setLayoutCallback(LayoutCallback callback) { m_layoutCallback = std::move(callback); }
  void setRedrawCallback(RedrawCallback callback) { m_redrawCallback = std::move(callback); }
  void setFrameTickRequestCallback(FrameTickRequestCallback callback) {
    m_frameTickRequestCallback = std::move(callback);
  }
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  // Desktop widget editor keeps widgets visible for layout even when runtime idle-hide applies.
  virtual void setEditorPreview(bool enabled) noexcept { (void)enabled; }
  void setBackgroundStyle(const ColorSpec& color, float radius, float padding);

  virtual bool applySetting(
      const std::string& key, const WidgetSettingValue& value,
      const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
  );

protected:
  void setRoot(std::unique_ptr<Node> root);

  void requestUpdate() {
    if (m_updateCallback) {
      m_updateCallback();
    }
  }

  void requestLayout() {
    if (m_layoutCallback) {
      m_layoutCallback();
    }
  }

  void requestRedraw() {
    if (m_redrawCallback) {
      m_redrawCallback();
    }
  }

  void requestFrameTick() {
    if (m_frameTickRequestCallback) {
      m_frameTickRequestCallback();
    }
  }

  virtual void doLayout(Renderer& renderer) = 0;
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

  float m_contentScale = 1.0f;
  AnimationManager* m_animations = nullptr;

private:
  void applyBackground();

  std::unique_ptr<Node> m_outerRoot;
  std::unique_ptr<Node> m_contentOwned;
  Node* m_contentRoot = nullptr;
  Node* m_outerRootPtr = nullptr;
  Box* m_bgBox = nullptr;
  UpdateCallback m_updateCallback;
  LayoutCallback m_layoutCallback;
  RedrawCallback m_redrawCallback;
  FrameTickRequestCallback m_frameTickRequestCallback;

  bool m_bgEnabled = false;
  ColorSpec m_bgColor;
  float m_bgRadius = 0.0f;
  float m_bgPadding = 0.0f;
};
