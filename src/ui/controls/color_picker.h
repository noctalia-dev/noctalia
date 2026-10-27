#pragma once

#include "render/core/color.h"
#include "render/scene/input_area.h"
#include "ui/controls/flex.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Button;
class Box;
class Image;
class Input;
class Label;
class Renderer;
class Select;

// HSV color picker: a 5:3 saturation/value plane, a hue strip, and an optional alpha strip
// (all textures), then a row with a format switch (Hex, RGB, HSL) and the fields of the
// chosen format, and optional swatches.
class ColorPicker : public Flex {
public:
  // Which text fields show: the format switch with its fields, the hex field alone, or none.
  enum class Inputs : std::uint8_t { Full, Hex, None };

  ColorPicker();

  void setScale(float scale);
  void setPickerWidth(float width);
  // Takes the width the parent offers instead of a fixed one.
  void setFillWidth();

  void setColor(const Color& rgba);
  [[nodiscard]] const Color& color() const noexcept { return m_color; }

  // Fires continuously while the color changes (every drag step, every applied keystroke).
  void setOnColorChanged(std::function<void(const Color&)> callback);
  // Fires once per finished change: a drag released, a typed value applied, a swatch clicked.
  void setOnColorCommitted(std::function<void(const Color&)> callback);

  void setAlphaEnabled(bool enabled);
  [[nodiscard]] bool alphaEnabled() const noexcept { return m_alphaEnabled; }
  void setInputs(Inputs inputs);
  // Clicking a swatch selects its color and keeps the current alpha.
  void setSwatches(std::vector<Color> swatches);

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  // True while the plane or a strip is held, so a host does not overwrite a drag in progress.
  [[nodiscard]] bool dragging() const noexcept;

  // Height of the default configuration (format switch, no alpha strip, no swatches).
  [[nodiscard]] static float intrinsicColumnHeight(float pickerWidth, float scale);
  [[nodiscard]] InputArea* primaryInputArea() const noexcept;

private:
  enum class Format : std::uint8_t { Hex, Rgb, Hsl };
  // Which fields a change came from; those fields are left alone while the user types.
  enum class Origin : std::uint8_t { Pointer, Hex, Components, Alpha };

  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  void rebuildSvTexture(Renderer& renderer);
  void rebuildHueTexture(Renderer& renderer);
  void rebuildAlphaTexture(Renderer& renderer);
  void rebuildSwatches();
  void applyStyle();
  void setFormat(Format format);
  void syncFields(Origin origin);
  // The three field values of the RGB or HSL format; RGB when the format is hex.
  [[nodiscard]] std::array<int, 3> componentValues() const;
  [[nodiscard]] std::string colorText() const;
  void positionOverlays();
  void updateHueThumbStyle();
  // Shared drag wiring for the plane and the strips; a release after any pick commits.
  InputArea* addPickArea(const std::function<void(const InputArea::PointerData&)>& pick);
  void applyPickFromSv(float localX, float localY);
  void applyPickFromHue(float localX);
  void applyPickFromAlpha(float localX);
  void onHexInputChange(const std::string& value);
  void applyHexInput(const std::string& value);
  void onComponentInputChange();
  void onAlphaInputChange();
  void commit(const Color& rgb, Origin origin);
  void notifyChanged();
  void notifyCommitted();

  float m_scale = 1.0F;
  float m_pickerWidth = 240.0F;
  bool m_fillWidth = false;
  float m_h = 0.0F;
  float m_s = 1.0F;
  float m_v = 1.0F;
  float m_alpha = 1.0F;
  Color m_color = rgba(1, 1, 1, 1);
  Format m_format = Format::Hex;
  bool m_suppressFieldCallbacks = false;
  bool m_alphaEnabled = false;
  Inputs m_inputs = Inputs::Full;
  // A pointer pick changed the color since the last press; its release commits.
  bool m_pickPending = false;

  static constexpr int kSvTextureSize = 192;
  static constexpr int kHueTextureWidth = 256;
  Image* m_svImage = nullptr;
  std::vector<std::uint8_t> m_svPixels;
  bool m_svTextureDirty = true;
  Image* m_hueImage = nullptr;
  bool m_hueTextureReady = false;
  Image* m_alphaImage = nullptr;
  std::vector<std::uint8_t> m_alphaPixels;
  bool m_alphaTextureDirty = true;
  int m_alphaTextureWidth = 0;
  int m_alphaTextureHeight = 0;

  Flex* m_fieldsRow = nullptr;
  Select* m_formatSelect = nullptr;
  Input* m_hexInput = nullptr;
  std::array<Input*, 3> m_componentInputs{};
  Input* m_alphaField = nullptr;
  Button* m_copyButton = nullptr;

  Flex* m_swatchRow = nullptr;
  std::vector<Color> m_swatches;
  std::vector<Box*> m_swatchBoxes;

  InputArea* m_svInput = nullptr;
  InputArea* m_hueInput = nullptr;
  InputArea* m_alphaInput = nullptr;
  Box* m_svThumb = nullptr;
  Box* m_hueThumb = nullptr;
  Box* m_alphaThumb = nullptr;

  std::function<void(const Color&)> m_onColorChanged;
  std::function<void(const Color&)> m_onColorCommitted;
  bool m_enabled = true;
};

// Title + ColorPicker + Cancel/Apply row — reusable chrome for dialogs or embedded UIs.
class ColorPickerSheet : public Flex {
public:
  explicit ColorPickerSheet(float chromeScale);

  [[nodiscard]] ColorPicker* colorPicker() noexcept { return m_picker; }
  [[nodiscard]] InputArea* initialFocusArea() const noexcept;

  void setTitle(std::string_view title);
  void setPickerColumnWidth(float width);

  void setOnCancel(std::function<void()> callback) { m_onCancel = std::move(callback); }
  void setOnApply(std::function<void(const Color&)> callback) { m_onApply = std::move(callback); }

  [[nodiscard]] static float preferredDialogWidth(float scale);
  [[nodiscard]] static float preferredDialogHeight(float dialogWidth, float scale);

private:
  float m_chromeScale = 1.0F;
  Label* m_title = nullptr;
  ColorPicker* m_picker = nullptr;
  std::function<void()> m_onCancel;
  std::function<void(const Color&)> m_onApply;
};
