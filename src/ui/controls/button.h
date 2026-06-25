#pragma once

#include "render/core/color.h"
#include "ui/controls/flex.h"
#include "ui/signal.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class Glyph;
class InputArea;
class Label;

enum class ButtonContentAlign : std::uint8_t {
  Center,
  Start,
  End,
};

enum class ButtonVariant : std::uint8_t {
  Default,
  Primary,
  Secondary,
  Destructive,
  Outline,
  Ghost,
  Tab,
  TabActive,
};

class Button : public Flex {
public:
  struct ButtonStateColors {
    ColorSpec bg;
    ColorSpec border;
    ColorSpec label;
  };

  struct ButtonPalette {
    float borderWidth = 0.0f;
    ButtonStateColors normal;
    ButtonStateColors hover;
    ButtonStateColors pressed;
    ButtonStateColors disabled;
    std::optional<ButtonStateColors> selected;
  };

  Button();
  ~Button() override;

  void setText(std::string_view text);
  void setGlyph(std::string_view name);
  void setFontSize(float size);
  void setGlyphSize(float size);
  // Pin the button to an exact height (e.g. Style::controlHeightSm) instead of the content-derived
  // default, so it can sit on the same size tier as the Select/Input controls beside it.
  void setControlHeight(float height);
  void setEnabled(bool enabled);
  void setSelected(bool selected);
  void setContentAlign(ButtonContentAlign align);
  void setVariant(ButtonVariant variant);
  void setCustomPalette(ButtonPalette customPalette);
  void setSurfaceOpacity(float opacity);
  void setOnClick(std::function<void()> callback);
  void setOnRightClick(std::function<void()> callback);
  void setOnPress(std::function<void(float localX, float localY, bool pressed)> callback);
  void setOnMotion(std::function<void()> callback);
  void setOnPointerMotion(std::function<void(float localX, float localY)> callback);
  void setOnEnter(std::function<void()> callback);
  void setOnLeave(std::function<void()> callback);
  void setHoverSuppressed(bool suppressed);
  void setHoveredVisual(bool hovered);
  void setPressedVisual(bool pressed);
  void setCursorShape(std::uint32_t shape);
  void setBadge(std::string_view text);
  void setBadgeFontSize(float size);
  void setTooltip(std::string_view text);

  void setTabStop(bool tabStop);
  void setKeyboardFocusHint(bool hint);

  // Call after layout() to sync InputArea bounds
  void updateInputArea();

  [[nodiscard]] Label* label() const noexcept { return m_label; }
  [[nodiscard]] Glyph* glyph() const noexcept { return m_glyph; }
  [[nodiscard]] InputArea* inputArea() const noexcept { return m_inputArea; }
  [[nodiscard]] bool hovered() const noexcept;
  [[nodiscard]] bool pressed() const noexcept;
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] ButtonVariant variant() const noexcept { return m_variant; }

  [[nodiscard]] static ButtonPalette defaultPalette(ButtonVariant variant);

private:
  void refreshInputAreaEnabled();
  void ensureLabel();
  void ensureGlyph();
  void applyVariant();
  void applyVisualState();
  void resolveVisualStateColors(Color& bg, Color& border, Color& label) const;
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  void applyColors(const Color& bg, const Color& border, const Color& label);

  // Constrain the label to the button's max width (minus padding/glyph) and ellipsize on overflow.
  void applyLabelMaxWidth();

  void ensureBadge();

  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  Flex* m_badge = nullptr;
  Label* m_badgeLabel = nullptr;
  InputArea* m_inputArea = nullptr;
  std::uint32_t m_animId = 0;
  std::function<void()> m_onClick;
  std::function<void()> m_onRightClick;
  std::function<void(float, float, bool)> m_onPress;
  std::function<void()> m_onMotion;
  std::function<void(float, float)> m_onPointerMotion;
  std::function<void()> m_onEnter;
  std::function<void()> m_onLeave;
  ButtonVariant m_variant = ButtonVariant::Default;
  ButtonPalette m_palette;
  std::optional<ButtonPalette> m_customPalette;
  // Animation: snapshot of colors at transition start
  Color m_fromBg{};
  Color m_fromBorder{};
  Color m_fromLabel{};
  Color m_targetBg{};
  Color m_targetBorder{};
  Color m_targetLabel{};
  ButtonContentAlign m_contentAlign = ButtonContentAlign::Center;
  float m_surfaceOpacity = 1.0f;
  bool m_enabled = true;
  bool m_selected = false;
  bool m_keyboardFocusHint = false;
  bool m_hoverSuppressed = false;
  bool m_hoveredVisual = false;
  bool m_pressedVisual = false;
  bool m_visualStateInitialized = false;
  Signal<>::ScopedConnection m_paletteConn;
};

class Renderer;

// returns rows of buttons packed to fit maxWidth, gap between buttons
std::vector<std::vector<std::unique_ptr<Button>>>
wrapButtonsIntoRows(Renderer& renderer, std::vector<std::unique_ptr<Button>>& buttons, float maxWidth, float gap);

// Populates a container column with row sub-containers, applying layout alignment,
// gaps, setFlexGrow(1.0f), and setMaxWidth for single-button rows.
void populateRowContainer(
    Flex& container, std::vector<std::vector<std::unique_ptr<Button>>> rows, float maxWidth, float gap
);
