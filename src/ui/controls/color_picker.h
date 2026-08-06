#pragma once

#include "render/core/color.h"
#include "ui/controls/flex.h"

#include <cstdint>
#include <functional>
#include <vector>

class Box;
class Image;
class Input;
class InputArea;
class Label;
class Renderer;

// HSV color picker: continuous SV plane (RGBA texture), hue strip, Hex + R/G/B inputs.
class ColorPicker : public Flex {
public:
  ColorPicker();

  void setScale(float scale);
  void setPickerWidth(float width);

  void setColor(const Color& rgba);
  [[nodiscard]] const Color& color() const noexcept { return m_color; }

  void setOnColorChanged(std::function<void(const Color&)> callback);

  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  [[nodiscard]] static float intrinsicColumnHeight(float pickerWidth, float scale);
  [[nodiscard]] InputArea* primaryInputArea() const noexcept;

private:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  [[nodiscard]] static Color colorAtSv(float h, float s, float v);
  void rebuildSvTexture(Renderer& renderer);
  void syncFieldsFromColor();
  void positionOverlays();
  void updateHueThumbStyle();
  void applyPickFromSv(float localX, float localY);
  void applyPickFromHue(float localX);
  void onHexInputChange(const std::string& value);
  void applyHexInput(const std::string& value);
  void onRgbInputChange();

  float m_scale = 1.0F;
  float m_pickerWidth = 240.0F;
  float m_h = 0.0F;
  float m_s = 1.0F;
  float m_v = 1.0F;
  float m_alpha = 1.0F;
  Color m_color = rgba(1, 1, 1, 1);
  bool m_suppressFieldCallbacks = false;

  static constexpr int kSvTextureSize = 192;
  Image* m_svImage = nullptr;
  std::vector<std::uint8_t> m_svPixels;
  bool m_svTextureDirty = true;

  Flex* m_hueStrip = nullptr;
  Flex* m_fieldsRow = nullptr;
  Input* m_hexInput = nullptr;
  Input* m_rInput = nullptr;
  Input* m_gInput = nullptr;
  Input* m_bInput = nullptr;

  InputArea* m_svInput = nullptr;
  InputArea* m_hueInput = nullptr;
  Box* m_svThumb = nullptr;
  Box* m_hueThumb = nullptr;

  std::function<void(const Color&)> m_onColorChanged;
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
