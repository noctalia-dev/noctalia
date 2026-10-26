#include "ui/controls/color_picker.h"

#include "core/text_clipboard.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/clamp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr std::size_t kHexFieldMaxLen = 7; // '#' + 6 digits
  // The saturation/value plane is 5:3, wide rather than square, to keep the picker short.
  constexpr float kPlaneAspect = 0.6F;
  constexpr float kStripHeight = 12.0F;
  constexpr float kSvThumbSize = 18.0F;
  constexpr float kHueThumbSize = 16.0F;
  constexpr float kFormatSelectWidth = 76.0F;
  constexpr float kAlphaFieldWidth = 56.0F;
  constexpr float kSwatchSize = 24.0F;

  [[nodiscard]] std::string sanitizeHexFieldInput(std::string_view input) {
    std::string out;
    out.reserve(kHexFieldMaxLen);
    bool hasHash = false;
    std::size_t digitCount = 0;
    for (const char c : input) {
      if (c == '#' && !hasHash) {
        hasHash = true;
        out.push_back('#');
        continue;
      }
      if (std::isxdigit(static_cast<unsigned char>(c)) != 0 && digitCount < 6) {
        if (!hasHash) {
          out.push_back('#');
          hasHash = true;
        }
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        ++digitCount;
      }
    }
    return out;
  }

  [[nodiscard]] bool isCompleteHexField(std::string_view value) {
    return value.size() == kHexFieldMaxLen && value.front() == '#';
  }

  // A whole field holding an integer in [lo, hi]; partial or out-of-range input is not a value.
  [[nodiscard]] std::optional<int> parseComponent(std::string_view text, int lo, int hi) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value < lo || value > hi) {
      return std::nullopt;
    }
    return value;
  }

  [[nodiscard]] int toByte(float channel) {
    return static_cast<int>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
  }

  [[nodiscard]] std::uint8_t toPixel(float channel) { return static_cast<std::uint8_t>(toByte(channel)); }

} // namespace

float ColorPicker::intrinsicColumnHeight(float pickerWidth, float scale) {
  const float s = std::max(0.1F, scale);
  const float pw = std::max(120.0F, pickerWidth);
  return std::round(pw * kPlaneAspect) + kStripHeight * s + Style::controlHeightSm * s + 2.0F * Style::spaceSm * s;
}

ColorPicker::ColorPicker() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  setPadding(0.0F);

  addChild(
      ui::image({
          .out = &m_svImage,
          .fit = ImageFit::Stretch,
          .configure = [](Image& image) { image.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth); },
      })
  );
  addChild(ui::image({.out = &m_hueImage, .fit = ImageFit::Stretch}));
  addChild(ui::image({.out = &m_alphaImage, .fit = ImageFit::Stretch, .visible = false}));

  auto fields = ui::row({
      .out = &m_fieldsRow,
      .align = FlexAlign::Center,
      .gap = Style::spaceXs,
  });
  fields->addChild(
      ui::select({
          .out = &m_formatSelect,
          .options = std::vector<std::string>{"HEX", "RGB", "HSL"},
          .selectedIndex = 0,
          .fontSize = Style::fontSizeCaption,
          .controlHeight = Style::controlHeightSm,
          .horizontalPadding = Style::spaceSm,
          .width = kFormatSelectWidth,
          .onSelectionChanged = [this](std::size_t index, std::string_view /*text*/) {
            setFormat(static_cast<Format>(index));
          },
      })
  );
  const auto makeField = [](Input** out) {
    return ui::input({
        .out = out,
        .fontSize = Style::fontSizeCaption,
        .controlHeight = Style::controlHeightSm,
        .horizontalPadding = Style::spaceSm,
        .flexGrow = 1.0F,
    });
  };
  fields->addChild(makeField(&m_hexInput));
  for (Input*& component : m_componentInputs) {
    fields->addChild(makeField(&component));
    component->setOnChange([this](const std::string& /*value*/) { onComponentInputChange(); });
  }
  fields->addChild(
      ui::input({
          .out = &m_alphaField,
          .fontSize = Style::fontSizeCaption,
          .controlHeight = Style::controlHeightSm,
          .horizontalPadding = Style::spaceSm,
          .width = kAlphaFieldWidth,
          .visible = false,
      })
  );
  fields->addChild(
      ui::button({
          .out = &m_copyButton,
          .glyph = "copy",
          .glyphSize = Style::fontSizeCaption,
          .controlHeight = Style::controlHeightSm,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("ui.dialogs.color-picker.copy"),
          .onClick = [this]() {
            if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
              clipboard->setClipboardText(colorText());
            }
          },
      })
  );
  m_alphaField->setOnChange([this](const std::string& /*value*/) { onAlphaInputChange(); });
  m_hexInput->setOnChange([this](const std::string& value) { onHexInputChange(value); });
  m_hexInput->setOnSubmit([this](const std::string& value) { applyHexInput(sanitizeHexFieldInput(value)); });
  m_hexInput->setSubmitOnFocusLoss(true);
  addChild(std::move(fields));
  addChild(ui::row({.out = &m_swatchRow, .align = FlexAlign::Center, .gap = Style::spaceXs, .visible = false}));

  m_svInput = addPickArea([this](const InputArea::PointerData& data) { applyPickFromSv(data.localX, data.localY); });
  m_hueInput = addPickArea([this](const InputArea::PointerData& data) { applyPickFromHue(data.localX); });
  m_alphaInput = addPickArea([this](const InputArea::PointerData& data) { applyPickFromAlpha(data.localX); });
  m_alphaInput->setVisible(false);

  addChild(
      ui::box({
          .out = &m_svThumb,
          .fill = fixedColorSpec(rgba(0.0F, 0.0F, 0.0F, 0.0F)),
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(6); },
      })
  );
  addChild(
      ui::box({
          .out = &m_hueThumb,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(6); },
      })
  );
  addChild(
      ui::box({
          .out = &m_alphaThumb,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(6); },
      })
  );

  applyStyle();
  setFormat(Format::Hex);
}

bool ColorPicker::dragging() const noexcept {
  return m_svInput->pressed() || m_hueInput->pressed() || m_alphaInput->pressed();
}

InputArea* ColorPicker::primaryInputArea() const noexcept {
  return m_hexInput != nullptr ? m_hexInput->inputArea() : nullptr;
}

InputArea* ColorPicker::addPickArea(std::function<void(const InputArea::PointerData&)> pick) {
  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(10);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  InputArea* raw = area.get();
  area->setOnPress([this, pick](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      pick(data);
    } else if (std::exchange(m_pickPending, false)) {
      notifyCommitted();
    }
  });
  area->setOnMotion([raw, pick](const InputArea::PointerData& data) {
    if (raw->pressed()) {
      pick(data);
    }
  });
  area->setOnCancel([this] {
    if (std::exchange(m_pickPending, false)) {
      notifyCommitted();
    }
  });
  return static_cast<InputArea*>(addChild(std::move(area)));
}

void ColorPicker::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applyStyle();
  markLayoutDirty();
}

void ColorPicker::applyStyle() {
  setGap(Style::spaceSm * m_scale);
  m_fieldsRow->setGap(Style::spaceXs * m_scale);
  m_swatchRow->setGap(Style::spaceXs * m_scale);
  const float fontSize = Style::fontSizeCaption * m_scale;
  const float controlHeight = Style::controlHeightSm * m_scale;
  const float padding = Style::spaceSm * m_scale;
  m_formatSelect->setFontSize(fontSize);
  m_formatSelect->setControlHeight(controlHeight);
  m_formatSelect->setHorizontalPadding(padding);
  m_formatSelect->setMinWidth(kFormatSelectWidth * m_scale);
  m_alphaField->setSize(kAlphaFieldWidth * m_scale, m_alphaField->height());
  for (Input* field : {m_hexInput, m_componentInputs[0], m_componentInputs[1], m_componentInputs[2], m_alphaField}) {
    field->setFontSize(fontSize);
    field->setControlHeight(controlHeight);
    field->setHorizontalPadding(padding);
  }
  m_copyButton->setGlyphSize(fontSize);
  m_copyButton->setControlHeight(controlHeight);
  const float svThumb = kSvThumbSize * m_scale;
  m_svThumb->setSize(svThumb, svThumb);
  m_svThumb->setRadius(svThumb * 0.5F);
  m_svThumb->setBorder(rgba(1.0F, 1.0F, 1.0F, 1.0F), 2.5F * m_scale);
  const float hueThumb = kHueThumbSize * m_scale;
  m_hueThumb->setSize(hueThumb, hueThumb);
  m_hueThumb->setRadius(hueThumb * 0.5F);
  m_alphaThumb->setSize(hueThumb, hueThumb);
  m_alphaThumb->setRadius(hueThumb * 0.5F);
  for (Box* swatch : m_swatchBoxes) {
    swatch->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * m_scale);
  }
  m_svImage->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * m_scale);
  updateHueThumbStyle();
}

void ColorPicker::setPickerWidth(float width) {
  m_fillWidth = false;
  m_pickerWidth = std::max(120.0F, width);
  setMinWidth(m_pickerWidth);
  markLayoutDirty();
}

void ColorPicker::setFillWidth() {
  if (m_fillWidth) {
    return;
  }
  m_fillWidth = true;
  setMinWidth(0.0F);
  markLayoutDirty();
}

void ColorPicker::setColor(const Color& rgba) {
  m_alpha = rgba.a;
  commit(rgba, Origin::Pointer);
}

void ColorPicker::commit(const Color& rgb, Origin origin) {
  float h = 0.0F;
  rgbToHsv(rgb, h, m_s, m_v);
  // Grays have no hue: keep the strip where the user left it.
  if (m_s > 1e-4F && m_v > 1e-4F) {
    m_h = h;
  }
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  m_svTextureDirty = true;
  m_alphaTextureDirty = true;
  updateHueThumbStyle();
  syncFields(origin);
  markPaintDirty();
  markLayoutDirty();
}

void ColorPicker::setOnColorChanged(std::function<void(const Color&)> callback) {
  m_onColorChanged = std::move(callback);
}

void ColorPicker::notifyChanged() {
  if (m_onColorChanged) {
    m_onColorChanged(m_color);
  }
}

void ColorPicker::setOnColorCommitted(std::function<void(const Color&)> callback) {
  m_onColorCommitted = std::move(callback);
}

void ColorPicker::notifyCommitted() {
  if (m_onColorCommitted) {
    m_onColorCommitted(m_color);
  }
}

void ColorPicker::setAlphaEnabled(bool enabled) {
  if (m_alphaEnabled == enabled) {
    return;
  }
  m_alphaEnabled = enabled;
  m_alphaImage->setVisible(enabled);
  m_alphaInput->setVisible(enabled);
  m_alphaThumb->setVisible(enabled);
  m_alphaField->setVisible(enabled);
  m_alphaTextureDirty = true;
  syncFields(Origin::Pointer);
  markLayoutDirty();
}

void ColorPicker::setInputs(Inputs inputs) {
  if (m_inputs == inputs) {
    return;
  }
  m_inputs = inputs;
  m_fieldsRow->setVisible(inputs != Inputs::None);
  m_formatSelect->setVisible(inputs == Inputs::Full);
  if (inputs == Inputs::Hex) {
    m_formatSelect->setSelectedIndexSilently(0);
    setFormat(Format::Hex);
  }
  markLayoutDirty();
}

void ColorPicker::setSwatches(std::vector<Color> swatches) {
  if (swatches == m_swatches) {
    return;
  }
  m_swatches = std::move(swatches);
  rebuildSwatches();
}

void ColorPicker::rebuildSwatches() {
  while (!m_swatchRow->children().empty()) {
    (void)m_swatchRow->removeChild(m_swatchRow->children().back().get());
  }
  m_swatchBoxes.clear();
  for (std::size_t i = 0; i < m_swatches.size(); ++i) {
    auto area = std::make_unique<InputArea>();
    area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    area->setEnabled(m_enabled);
    area->setOnClick([this, i](const InputArea::PointerData& /*data*/) {
      if (i >= m_swatches.size()) {
        return;
      }
      // A swatch picks the color; the transparency stays where the user set it.
      commit(m_swatches[i], Origin::Pointer);
      notifyChanged();
      notifyCommitted();
    });
    Box* box = nullptr;
    area->addChild(
        ui::box({
            .out = &box,
            .fill = fixedColorSpec(m_swatches[i]),
            .configure = [this](Box& swatch) {
              swatch.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * m_scale);
            },
        })
    );
    m_swatchBoxes.push_back(box);
    m_swatchRow->addChild(std::move(area));
  }
  m_swatchRow->setVisible(!m_swatches.empty());
  markLayoutDirty();
}

void ColorPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  m_svInput->setEnabled(enabled);
  m_hueInput->setEnabled(enabled);
  m_alphaInput->setEnabled(enabled);
  for (const auto& child : m_swatchRow->children()) {
    static_cast<InputArea*>(child.get())->setEnabled(enabled);
  }
  m_formatSelect->setEnabled(enabled);
  for (Input* field : {m_hexInput, m_componentInputs[0], m_componentInputs[1], m_componentInputs[2], m_alphaField}) {
    field->setEnabled(enabled);
  }
  m_copyButton->setEnabled(enabled);
  setOpacity(enabled ? 1.0F : 0.55F);
}

void ColorPicker::setFormat(Format format) {
  m_format = format;
  const bool hex = format == Format::Hex;
  m_hexInput->setVisible(hex);
  static constexpr std::array<std::string_view, 3> kRgb = {"R", "G", "B"};
  static constexpr std::array<std::string_view, 3> kHsl = {"H", "S", "L"};
  for (std::size_t i = 0; i < m_componentInputs.size(); ++i) {
    m_componentInputs[i]->setVisible(!hex);
    m_componentInputs[i]->setPlaceholder(format == Format::Hsl ? kHsl[i] : kRgb[i]);
    m_componentInputs[i]->setInvalid(false);
  }
  syncFields(Origin::Pointer);
  markLayoutDirty();
}

void ColorPicker::syncFields(Origin origin) {
  m_suppressFieldCallbacks = true;
  if (origin != Origin::Hex) {
    m_hexInput->setValue(formatRgbHex(m_color));
    m_hexInput->setInvalid(false);
  }
  if (origin != Origin::Components && m_format != Format::Hex) {
    const std::array<int, 3> values = componentValues();
    for (std::size_t i = 0; i < values.size(); ++i) {
      m_componentInputs[i]->setValue(std::to_string(values[i]));
      m_componentInputs[i]->setInvalid(false);
    }
  }
  if (origin != Origin::Alpha) {
    m_alphaField->setValue(std::format("{}%", static_cast<int>(std::lround(m_alpha * 100.0F))));
    m_alphaField->setInvalid(false);
  }
  m_suppressFieldCallbacks = false;
}

std::array<int, 3> ColorPicker::componentValues() const {
  if (m_format != Format::Hsl) {
    return {toByte(m_color.r), toByte(m_color.g), toByte(m_color.b)};
  }
  float h = 0.0F;
  float s = 0.0F;
  float l = 0.0F;
  rgbToHsl(m_color, h, s, l);
  // A gray reports the hue the strip still shows, so switching back keeps it.
  if (s <= 1e-4F) {
    h = m_h * 360.0F;
  }
  return {
      static_cast<int>(std::lround(h)) % 360, static_cast<int>(std::lround(s * 100.0F)),
      static_cast<int>(std::lround(l * 100.0F))
  };
}

// The color in the format the fields show, as CSS notation.
std::string ColorPicker::colorText() const {
  if (m_format == Format::Hex) {
    return m_alphaEnabled ? formatRgbaHex(m_color) : formatRgbHex(m_color);
  }
  const auto [a, b, c] = componentValues();
  const std::string alpha = m_alphaEnabled ? std::format(", {}%", static_cast<int>(std::lround(m_alpha * 100.0F))) : "";
  if (m_format == Format::Rgb) {
    return std::format("rgb({}, {}, {}{})", a, b, c, alpha);
  }
  return std::format("hsl({}, {}%, {}%{})", a, b, c, alpha);
}

void ColorPicker::rebuildSvTexture(Renderer& renderer) {
  if (!m_svTextureDirty) {
    return;
  }
  constexpr int size = kSvTextureSize;
  m_svPixels.resize(static_cast<std::size_t>(size) * size * 4U);
  for (int y = 0; y < size; ++y) {
    const float v = 1.0F - (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
    for (int x = 0; x < size; ++x) {
      const float s = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
      const Color px = hsv(m_h, s, v);
      const std::size_t o = (static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x)) * 4U;
      m_svPixels[o + 0U] = toPixel(px.r);
      m_svPixels[o + 1U] = toPixel(px.g);
      m_svPixels[o + 2U] = toPixel(px.b);
      m_svPixels[o + 3U] = 255U;
    }
  }
  m_svImage->setSourceRaw(
      renderer, m_svPixels.data(), m_svPixels.size(), size, size, size * 4, PixmapFormat::RGBA, false
  );
  m_svTextureDirty = false;
}

void ColorPicker::rebuildAlphaTexture(Renderer& renderer) {
  const int width = std::max(1, static_cast<int>(std::lround(m_alphaImage->width())));
  const int height = std::max(1, static_cast<int>(std::lround(m_alphaImage->height())));
  if (!m_alphaTextureDirty && width == m_alphaTextureWidth && height == m_alphaTextureHeight) {
    return;
  }
  // Built at the strip's own size so the checkerboard cells stay square.
  const int cell = std::max(1, height / 2);
  m_alphaPixels.resize(static_cast<std::size_t>(width) * height * 4U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float t = width > 1 ? static_cast<float>(x) / static_cast<float>(width - 1) : 1.0F;
      const float checker = ((x / cell) + (y / cell)) % 2 == 0 ? 1.0F : 0.8F;
      const std::size_t o = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4U;
      m_alphaPixels[o + 0U] = toPixel(checker + (m_color.r - checker) * t);
      m_alphaPixels[o + 1U] = toPixel(checker + (m_color.g - checker) * t);
      m_alphaPixels[o + 2U] = toPixel(checker + (m_color.b - checker) * t);
      m_alphaPixels[o + 3U] = 255U;
    }
  }
  m_alphaImage->setSourceRaw(
      renderer, m_alphaPixels.data(), m_alphaPixels.size(), width, height, width * 4, PixmapFormat::RGBA, false
  );
  m_alphaTextureDirty = false;
  m_alphaTextureWidth = width;
  m_alphaTextureHeight = height;
}

void ColorPicker::rebuildHueTexture(Renderer& renderer) {
  if (m_hueTextureReady) {
    return;
  }
  std::array<std::uint8_t, static_cast<std::size_t>(kHueTextureWidth) * 4U> pixels{};
  for (int x = 0; x < kHueTextureWidth; ++x) {
    const Color px = hsv(static_cast<float>(x) / static_cast<float>(kHueTextureWidth - 1), 1.0F, 1.0F);
    const auto o = static_cast<std::size_t>(x) * 4U;
    pixels[o + 0U] = toPixel(px.r);
    pixels[o + 1U] = toPixel(px.g);
    pixels[o + 2U] = toPixel(px.b);
    pixels[o + 3U] = 255U;
  }
  m_hueTextureReady = m_hueImage->setSourceRaw(
      renderer, pixels.data(), pixels.size(), kHueTextureWidth, 1, kHueTextureWidth * 4, PixmapFormat::RGBA, false
  );
}

void ColorPicker::updateHueThumbStyle() {
  m_hueThumb->setFill(hsv(m_h, 1.0F, 1.0F));
  m_hueThumb->setBorder(rgba(1.0F, 1.0F, 1.0F, 1.0F), 2.0F * m_scale);
}

void ColorPicker::applyPickFromSv(float localX, float localY) {
  const float w = m_svImage->width();
  const float h = m_svImage->height();
  if (w <= 0.0F || h <= 0.0F) {
    return;
  }
  m_s = std::clamp(localX / w, 0.0F, 1.0F);
  m_v = std::clamp(1.0F - localY / h, 0.0F, 1.0F);
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  m_alphaTextureDirty = true;
  m_pickPending = true;
  syncFields(Origin::Pointer);
  markPaintDirty();
  markLayoutDirty();
  positionOverlays();
  notifyChanged();
}

void ColorPicker::applyPickFromHue(float localX) {
  const float w = m_hueImage->width();
  if (w <= 0.0F) {
    return;
  }
  m_h = std::clamp(localX / w, 0.0F, 1.0F);
  m_svTextureDirty = true;
  m_alphaTextureDirty = true;
  m_pickPending = true;
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  syncFields(Origin::Pointer);
  updateHueThumbStyle();
  markPaintDirty();
  markLayoutDirty();
  positionOverlays();
  notifyChanged();
}

void ColorPicker::applyPickFromAlpha(float localX) {
  const float w = m_alphaImage->width();
  if (w <= 0.0F) {
    return;
  }
  m_alpha = std::clamp(localX / w, 0.0F, 1.0F);
  m_color.a = m_alpha;
  m_pickPending = true;
  syncFields(Origin::Pointer);
  markPaintDirty();
  positionOverlays();
  notifyChanged();
}

void ColorPicker::onAlphaInputChange() {
  if (m_suppressFieldCallbacks) {
    return;
  }
  std::string_view text = m_alphaField->value();
  if (text.ends_with('%')) {
    text.remove_suffix(1);
  }
  const auto percent = parseComponent(text, 0, 100);
  m_alphaField->setInvalid(!percent.has_value() && !text.empty());
  if (!percent.has_value()) {
    return;
  }
  m_alpha = static_cast<float>(*percent) / 100.0F;
  m_color.a = m_alpha;
  syncFields(Origin::Alpha);
  markPaintDirty();
  markLayoutDirty();
  notifyChanged();
  notifyCommitted();
}

void ColorPicker::applyHexInput(const std::string& value) {
  if (m_suppressFieldCallbacks) {
    return;
  }
  Color parsed{};
  if (!tryParseHexColor(value, parsed)) {
    m_hexInput->setInvalid(true);
    return;
  }
  m_hexInput->setInvalid(false);
  commit(parsed, Origin::Hex);
  notifyChanged();
  notifyCommitted();
}

void ColorPicker::onHexInputChange(const std::string& value) {
  if (m_suppressFieldCallbacks) {
    return;
  }
  const std::string sanitized = sanitizeHexFieldInput(value);
  if (sanitized != value) {
    m_suppressFieldCallbacks = true;
    m_hexInput->setValue(sanitized);
    m_suppressFieldCallbacks = false;
  }
  m_hexInput->setInvalid(false);
  if (isCompleteHexField(sanitized)) {
    applyHexInput(sanitized);
  }
}

void ColorPicker::onComponentInputChange() {
  if (m_suppressFieldCallbacks || m_format == Format::Hex) {
    return;
  }
  const bool isHsl = m_format == Format::Hsl;
  const std::array<int, 3> limits = isHsl ? std::array<int, 3>{359, 100, 100} : std::array<int, 3>{255, 255, 255};
  std::array<int, 3> values{};
  bool complete = true;
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto value = parseComponent(m_componentInputs[i]->value(), 0, limits[i]);
    // An empty field is mid-edit, not an error.
    m_componentInputs[i]->setInvalid(!value.has_value() && !m_componentInputs[i]->value().empty());
    complete = complete && value.has_value();
    values[i] = value.value_or(0);
  }
  if (!complete) {
    return;
  }
  const Color next = isHsl ? hsl(static_cast<float>(values[0]), static_cast<float>(values[1]) / 100.0F,
                                 static_cast<float>(values[2]) / 100.0F)
                           : rgba(
                                 static_cast<float>(values[0]) / 255.0F, static_cast<float>(values[1]) / 255.0F,
                                 static_cast<float>(values[2]) / 255.0F, 1.0F
                             );
  if (isHsl && values[1] == 0) {
    // A gray still remembers the typed hue.
    m_h = static_cast<float>(values[0]) / 360.0F;
  }
  commit(next, Origin::Components);
  notifyChanged();
  notifyCommitted();
}

void ColorPicker::positionOverlays() {
  m_svInput->setPosition(m_svImage->x(), m_svImage->y());
  m_svInput->setFrameSize(m_svImage->width(), m_svImage->height());
  m_hueInput->setPosition(m_hueImage->x(), m_hueImage->y());
  m_hueInput->setFrameSize(m_hueImage->width(), m_hueImage->height());
  m_alphaInput->setPosition(m_alphaImage->x(), m_alphaImage->y());
  m_alphaInput->setFrameSize(m_alphaImage->width(), m_alphaImage->height());

  const float planeW = m_svImage->width();
  const float planeH = m_svImage->height();
  m_svThumb->setPosition(
      m_svImage->x() + m_s * planeW - m_svThumb->width() * 0.5F,
      m_svImage->y() + (1.0F - m_v) * planeH - m_svThumb->height() * 0.5F
  );
  const float stripW = m_hueImage->width();
  const float x = util::clampOrdered(
      m_hueImage->x() + m_h * stripW - m_hueThumb->width() * 0.5F, m_hueImage->x(),
      m_hueImage->x() + stripW - m_hueThumb->width()
  );
  m_hueThumb->setPosition(x, m_hueImage->y() + (m_hueImage->height() - m_hueThumb->height()) * 0.5F);
  if (m_alphaEnabled) {
    const float alphaW = m_alphaImage->width();
    const float alphaX = util::clampOrdered(
        m_alphaImage->x() + m_alpha * alphaW - m_alphaThumb->width() * 0.5F, m_alphaImage->x(),
        m_alphaImage->x() + alphaW - m_alphaThumb->width()
    );
    m_alphaThumb->setPosition(alphaX, m_alphaImage->y() + (m_alphaImage->height() - m_alphaThumb->height()) * 0.5F);
    // The thumb shows the color itself; the strip behind it shows the transparency.
    m_alphaThumb->setFill(rgba(m_color.r, m_color.g, m_color.b, 1.0F));
    m_alphaThumb->setBorder(rgba(1.0F, 1.0F, 1.0F, 1.0F), 2.0F * m_scale);
  }
}

void ColorPicker::doLayout(Renderer& renderer) {
  const float pw = m_pickerWidth;
  const float stripH = kStripHeight * m_scale;
  // Read every layout so a corner-radius config change applies without a rebuild.
  m_svImage->setRadius(Style::scaledRadiusMd(m_scale));
  const float stripRadius = std::min(Style::scaledRadiusMd(m_scale), stripH * 0.5F);
  m_hueImage->setRadius(stripRadius);
  m_alphaImage->setRadius(stripRadius);

  m_svImage->setSize(pw, std::round(pw * kPlaneAspect));
  m_svImage->layout(renderer);
  rebuildSvTexture(renderer);
  m_hueImage->setSize(pw, stripH);
  m_hueImage->layout(renderer);
  rebuildHueTexture(renderer);
  if (m_alphaEnabled) {
    m_alphaImage->setSize(pw, stripH);
    m_alphaImage->layout(renderer);
    rebuildAlphaTexture(renderer);
  }
  if (!m_swatchBoxes.empty()) {
    // Swatches shrink to fit rather than wrap, so a long list stays one row.
    const float gap = Style::spaceXs * m_scale;
    const auto count = static_cast<float>(m_swatchBoxes.size());
    const float size = std::min(kSwatchSize * m_scale, (pw - gap * (count - 1.0F)) / count);
    for (Box* swatch : m_swatchBoxes) {
      swatch->setSize(size, size);
      swatch->setRadius(Style::scaledRadiusSm(m_scale));
      swatch->parent()->setSize(size, size);
    }
  }

  Flex::doLayout(renderer);
  positionOverlays();
}

LayoutSize ColorPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  if (m_fillWidth && constraints.hasMaxWidth && constraints.maxWidth > 0.0F) {
    m_pickerWidth = std::max(120.0F, constraints.maxWidth);
  }
  return measureByLayout(renderer, constraints);
}

void ColorPicker::doArrange(Renderer& renderer, const LayoutRect& rect) {
  if (m_fillWidth && rect.width > 0.0F) {
    m_pickerWidth = std::max(120.0F, rect.width);
  }
  arrangeByLayout(renderer, rect);
}

ColorPickerSheet::ColorPickerSheet(float chromeScale) : m_chromeScale(std::max(0.1F, chromeScale)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd * m_chromeScale);
  setPadding(Style::spaceSm * m_chromeScale);

  auto headerRow = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * m_chromeScale,
  });
  headerRow->addChild(
      ui::label({
          .out = &m_title,
          .text = i18n::tr("ui.dialogs.color-picker.title"),
          .fontSize = Style::fontSizeTitle * m_chromeScale,
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::Primary),
      })
  );
  headerRow->addChild(ui::spacer());
  if (Style::closeButtonsEnabled()) {
    headerRow->addChild(
        ui::button({
            .glyph = "close",
            .glyphSize = Style::fontSizeBody * m_chromeScale,
            .variant = ButtonVariant::Default,
            .minWidth = Style::controlHeightSm * m_chromeScale,
            .minHeight = Style::controlHeightSm * m_chromeScale,
            .padding = Style::spaceXs * m_chromeScale,
            .radius = Style::scaledRadiusMd(m_chromeScale),
            .onClick = [this]() {
              if (m_onCancel) {
                m_onCancel();
              }
            },
        })
    );
  }
  addChild(std::move(headerRow));

  auto picker = std::make_unique<ColorPicker>();
  picker->setScale(m_chromeScale);
  m_picker = picker.get();
  addChild(std::move(picker));

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .justify = FlexJustify::End,
              .gap = Style::spaceSm * m_chromeScale,
          },
          ui::button({
              .text = i18n::tr("common.actions.cancel"),
              .variant = ButtonVariant::Secondary,
              .minWidth = 92.0F * m_chromeScale,
              .minHeight = Style::controlHeight * m_chromeScale,
              .paddingV = Style::spaceSm * m_chromeScale,
              .paddingH = Style::spaceMd * m_chromeScale,
              .radius = Style::scaledRadiusMd(m_chromeScale),
              .onClick =
                  [this]() {
                    if (m_onCancel) {
                      m_onCancel();
                    }
                  },
          }),
          ui::button({
              .text = i18n::tr("common.actions.apply"),
              .variant = ButtonVariant::Primary,
              .minWidth = 92.0F * m_chromeScale,
              .minHeight = Style::controlHeight * m_chromeScale,
              .paddingV = Style::spaceSm * m_chromeScale,
              .paddingH = Style::spaceMd * m_chromeScale,
              .radius = Style::scaledRadiusMd(m_chromeScale),
              .onClick = [this]() {
                if (m_picker != nullptr && m_onApply) {
                  m_onApply(m_picker->color());
                }
              },
          })
      )
  );
}

InputArea* ColorPickerSheet::initialFocusArea() const noexcept {
  return m_picker != nullptr ? m_picker->primaryInputArea() : nullptr;
}

void ColorPickerSheet::setTitle(std::string_view title) {
  if (m_title != nullptr) {
    m_title->setText(title.empty() ? i18n::tr("ui.dialogs.color-picker.title") : std::string(title));
  }
}

void ColorPickerSheet::setPickerColumnWidth(float width) {
  if (m_picker != nullptr) {
    m_picker->setPickerWidth(width);
  }
}

float ColorPickerSheet::preferredDialogWidth(float scale) { return 340.0F * std::max(0.1F, scale); }

float ColorPickerSheet::preferredDialogHeight(float dialogWidth, float scale) {
  const float s = std::max(0.1F, scale);
  const float pad = Style::spaceSm * s;
  const float innerW = std::max(160.0F, dialogWidth - 2.0F * pad);
  const float pickerColH = ColorPicker::intrinsicColumnHeight(innerW, s);
  const float titleH = Style::fontSizeTitle * s * 1.3F;
  const float actionsH = Style::controlHeight * s + 2.0F * Style::spaceSm * s;
  const float gapMd = Style::spaceMd * s;
  return 2.0F * pad + titleH + pickerColH + actionsH + 2.0F * gapMd;
}
