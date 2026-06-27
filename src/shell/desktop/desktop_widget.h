#pragma once

#include "config/config_types.h"
#include "render/scene/node.h"
#include "ui/palette.h"

#include <cmath>
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

  virtual void layout(Renderer& renderer);
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
  void setContentScale(float scale) noexcept {
    if (scale == m_baseScale) {
      return;
    }
    m_baseScale = scale;
    m_contentScale = scale;
  }
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  // The aspect-preserved content scale that fits the natural-size high-water mark into the given
  // box, clamped to the fit limits.
  [[nodiscard]] float contentScaleForBox(float boxWidth, float boxHeight) const noexcept;
  // Target box size (logical px) of the widget's grid tile. 0 means auto-fit the content's
  // natural size. When set, layout() scales the content to fill the box (aspect-preserved,
  // centered) and the background/surface take the box dimensions.
  void setBox(float width, float height) noexcept {
    m_boxWidth = width;
    m_boxHeight = height;
  }
  // Font family for the widget's text (empty = inherit the shell font). Stored here and applied to
  // labels during layout() so it survives widget rebuilds, not just live setting changes.
  void setFontFamily(const std::string& family) { m_fontFamily = family; }
  // Desktop widget editor keeps widgets visible for layout even when runtime idle-hide applies.
  virtual void setEditorPreview(bool enabled) noexcept { (void)enabled; }
  void setBackgroundStyle(const ColorSpec& color, float radius, float padding);

  [[nodiscard]] bool hasBackground() const noexcept { return m_bgEnabled; }
  [[nodiscard]] float backgroundRadius() const noexcept {
    return m_bgEnabled ? std::round(m_bgRadius * m_baseScale) : 0.0f;
  }

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

  [[nodiscard]] float boxInnerWidth() const noexcept;
  [[nodiscard]] float boxInnerHeight() const noexcept;
  // True while layout() runs, including the nested update() calls a doLayout may make (which open
  // their own Update phase scope). Subclasses use this to avoid re-arming a layout from within one.
  [[nodiscard]] bool isLayingOut() const noexcept { return m_inLayout; }

  virtual void doLayout(Renderer& renderer) = 0;
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

  // Push the widget's configured font family onto every text node it owns. Empty family means
  // inherit the shell font. The base handles the `font_family` setting and the relayout; text
  // widgets override this to apply it to their labels.
  virtual void onFontFamilyChanged(const std::string& family, Renderer& renderer) {
    (void)family;
    (void)renderer;
  }

  // Outer node released to the host: background wrapper when enabled, otherwise content.
  [[nodiscard]] Node* presentationRoot() const noexcept;

  bool m_inLayout = false;
  float m_contentScale = 1.0f;
  float m_baseScale = 1.0f;
  float m_boxWidth = 0.0f;
  float m_boxHeight = 0.0f;
  std::string m_fontFamily; // empty = inherit the shell font
  // High-water marks of the natural content size (measured at base scale), so the box-fit factor
  // tracks the widest content ever seen rather than the live content. This keeps the font size
  // stable for dynamic text (e.g. a seconds clock in a proportional font) instead of breathing
  // every update. Reset when the base scale changes.
  float m_maxNaturalWidth = 0.0f;
  float m_maxNaturalHeight = 0.0f;
  float m_fitRefScale = -1.0f;
  AnimationManager* m_animations = nullptr;

  void applyBackground();

  std::unique_ptr<Node> m_outerRoot;
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
