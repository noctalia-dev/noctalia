#include "ui/controls/color_picker.h"

#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <string>

namespace {

  int parseIntClamp(const std::string& s, int lo, int hi) {
    try {
      const long v = std::stol(s);
      return static_cast<int>(std::clamp(v, static_cast<long>(lo), static_cast<long>(hi)));
    } catch (...) {
      return lo;
    }
  }

} // namespace

Color ColorPicker::colorAtSv(float h, float s, float v) { return hsv(h, s, v); }

float ColorPicker::intrinsicColumnHeight(float pickerWidth, float scale) {
  const float s = std::max(0.1f, scale);
  const float pw = std::max(120.0f, pickerWidth);
  const float stripH = std::max(10.0f, 12.0f * s);
  const float labelLine = Style::fontSizeCaption * s * 1.4f;
  const float fieldRowH = labelLine + Style::spaceXs * 0.5f * s + Style::controlHeightSm * s;
  const float gapSm = Style::spaceSm * s;
  return pw + stripH + fieldRowH + 2.0f * gapSm;
}

ColorPicker::ColorPicker() {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  setPadding(0.0f);

  addChild(
      ui::image({
          .out = &m_svImage,
          .fit = ImageFit::Stretch,
          .configure = [](Image& image) { image.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth); },
      })
  );

  constexpr int kHueSegments = 36;
  auto hueStrip = ui::row({
      .out = &m_hueStrip,
      .align = FlexAlign::Stretch,
      .gap = 0.0f,
  });
  for (int i = 0; i < kHueSegments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kHueSegments - 1);
    hueStrip->addChild(
        ui::box({
            .fill = fixedColorSpec(hsv(t, 1.0f, 1.0f)),
            .radius = 0.0f,
            .flexGrow = 1.0f,
            .configure = [](Box& box) { box.clearBorder(); },
        })
    );
  }
  addChild(std::move(hueStrip));

  auto fields = ui::row({
      .out = &m_fieldsRow,
      .align = FlexAlign::Center,
      .justify = FlexJustify::Start,
      .gap = Style::spaceSm,
  });

  auto addField = [this](const char* title, float w) {
    auto col = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * 0.5f,
    });
    col->addChild(
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeCaption,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
    Input* inp = nullptr;
    col->addChild(
        ui::input({
            .out = &inp,
            .fontSize = Style::fontSizeCaption,
            .controlHeight = Style::controlHeightSm,
            .horizontalPadding = Style::spaceSm,
            .width = w,
            .height = 0.0f,
        })
    );
    m_fieldsRow->addChild(std::move(col));
    return inp;
  };

  m_hexInput = addField("Hex", 108.0f);
  m_rInput = addField("R", 56.0f);
  m_gInput = addField("G", 56.0f);
  m_bInput = addField("B", 56.0f);

  m_hexInput->setOnChange([this](const std::string& v) { onHexInputChange(v); });
  m_rInput->setOnChange([this](const std::string& /*v*/) { onRgbInputChange(); });
  m_gInput->setOnChange([this](const std::string& /*v*/) { onRgbInputChange(); });
  m_bInput->setOnChange([this](const std::string& /*v*/) { onRgbInputChange(); });

  addChild(std::move(fields));

  auto svInput = std::make_unique<InputArea>();
  svInput->setParticipatesInLayout(false);
  svInput->setZIndex(10);
  svInput->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  svInput->setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed || data.button != BTN_LEFT) {
      return;
    }
    applyPickFromSv(data.localX, data.localY);
  });
  svInput->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_svInput == nullptr || !m_svInput->pressed()) {
      return;
    }
    applyPickFromSv(data.localX, data.localY);
  });
  m_svInput = static_cast<InputArea*>(addChild(std::move(svInput)));

  auto hueInput = std::make_unique<InputArea>();
  hueInput->setParticipatesInLayout(false);
  hueInput->setZIndex(10);
  hueInput->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  hueInput->setOnPress([this](const InputArea::PointerData& data) {
    if (!data.pressed || data.button != BTN_LEFT) {
      return;
    }
    applyPickFromHue(data.localX);
  });
  hueInput->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_hueInput == nullptr || !m_hueInput->pressed()) {
      return;
    }
    applyPickFromHue(data.localX);
  });
  m_hueInput = static_cast<InputArea*>(addChild(std::move(hueInput)));

  addChild(
      ui::box({
          .out = &m_svThumb,
          .fill = fixedColorSpec(rgba(0.0f, 0.0f, 0.0f, 0.0f)),
          .radius = 12.0f,
          .width = 24.0f,
          .height = 24.0f,
          .participatesInLayout = false,
          .configure = [](Box& box) {
            box.setZIndex(6);
            box.setBorder(rgba(1.0f, 1.0f, 1.0f, 1.0f), 2.5f);
          },
      })
  );

  addChild(
      ui::box({
          .out = &m_hueThumb,
          .radius = 10.0f,
          .width = 20.0f,
          .height = 20.0f,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(6); },
      })
  );

  m_svTextureDirty = true;
  updateHueThumbStyle();
  syncFieldsFromColor();
}

InputArea* ColorPicker::primaryInputArea() const noexcept {
  return m_hexInput != nullptr ? m_hexInput->inputArea() : nullptr;
}

void ColorPicker::setScale(float scale) {
  m_scale = std::max(0.1f, scale);
  setGap(Style::spaceSm * m_scale);
  if (m_fieldsRow != nullptr) {
    m_fieldsRow->setGap(Style::spaceSm * m_scale);
  }
  const float fs = Style::fontSizeCaption * m_scale;
  const float ch = Style::controlHeightSm * m_scale;
  const float pad = Style::spaceSm * m_scale;
  for (Input* in : {m_hexInput, m_rInput, m_gInput, m_bInput}) {
    if (in != nullptr) {
      in->setFontSize(fs);
      in->setControlHeight(ch);
      in->setHorizontalPadding(pad);
    }
  }
  if (m_svThumb != nullptr) {
    const float t = 24.0f * m_scale;
    m_svThumb->setSize(t, t);
    m_svThumb->setRadius(t * 0.5f);
    m_svThumb->setBorder(rgba(1.0f, 1.0f, 1.0f, 1.0f), 2.5f * m_scale);
  }
  if (m_hueThumb != nullptr) {
    const float t = 20.0f * m_scale;
    m_hueThumb->setSize(t, t);
    m_hueThumb->setRadius(t * 0.5f);
  }
  if (m_svImage != nullptr) {
    m_svImage->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth * m_scale);
  }
  markLayoutDirty();
}

void ColorPicker::setPickerWidth(float width) {
  m_pickerWidth = std::max(120.0f, width);
  setMinWidth(m_pickerWidth);
  markLayoutDirty();
}

void ColorPicker::setColor(const Color& rgba) {
  m_alpha = rgba.a;
  rgbToHsv(rgba, m_h, m_s, m_v);
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  m_svTextureDirty = true;
  updateHueThumbStyle();
  syncFieldsFromColor();
  markPaintDirty();
  markLayoutDirty();
}

void ColorPicker::setOnColorChanged(std::function<void(const Color&)> callback) {
  m_onColorChanged = std::move(callback);
}

void ColorPicker::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_svInput != nullptr) {
    m_svInput->setEnabled(enabled);
  }
  if (m_hueInput != nullptr) {
    m_hueInput->setEnabled(enabled);
  }
  for (Input* field : {m_hexInput, m_rInput, m_gInput, m_bInput}) {
    if (field != nullptr) {
      field->setEnabled(enabled);
    }
  }
  setOpacity(enabled ? 1.0f : 0.55f);
}

void ColorPicker::rebuildSvTexture(Renderer& renderer) {
  if (m_svImage == nullptr || !m_svTextureDirty) {
    return;
  }
  const int tw = kSvTextureSize;
  const int th = kSvTextureSize;
  const std::size_t bytes = static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4U;
  m_svPixels.resize(bytes);
  for (int y = 0; y < th; ++y) {
    const float v = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(th);
    for (int x = 0; x < tw; ++x) {
      const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(tw);
      const Color px = colorAtSv(m_h, s, v);
      const std::size_t o =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(tw) + static_cast<std::size_t>(x)) * 4U;
      m_svPixels[o + 0U] = static_cast<std::uint8_t>(std::lround(std::clamp(px.r, 0.0f, 1.0f) * 255.0f));
      m_svPixels[o + 1U] = static_cast<std::uint8_t>(std::lround(std::clamp(px.g, 0.0f, 1.0f) * 255.0f));
      m_svPixels[o + 2U] = static_cast<std::uint8_t>(std::lround(std::clamp(px.b, 0.0f, 1.0f) * 255.0f));
      m_svPixels[o + 3U] = 255U;
    }
  }
  m_svImage->setSourceRaw(renderer, m_svPixels.data(), m_svPixels.size(), tw, th, tw * 4, PixmapFormat::RGBA, false);
  m_svTextureDirty = false;
}

void ColorPicker::syncFieldsFromColor() {
  m_suppressFieldCallbacks = true;
  if (m_hexInput != nullptr) {
    m_hexInput->setValue(formatRgbHex(m_color));
  }
  if (m_rInput != nullptr) {
    m_rInput->setValue(std::to_string(static_cast<int>(std::lround(std::clamp(m_color.r, 0.0f, 1.0f) * 255.0f))));
  }
  if (m_gInput != nullptr) {
    m_gInput->setValue(std::to_string(static_cast<int>(std::lround(std::clamp(m_color.g, 0.0f, 1.0f) * 255.0f))));
  }
  if (m_bInput != nullptr) {
    m_bInput->setValue(std::to_string(static_cast<int>(std::lround(std::clamp(m_color.b, 0.0f, 1.0f) * 255.0f))));
  }
  m_suppressFieldCallbacks = false;
}

void ColorPicker::updateHueThumbStyle() {
  if (m_hueThumb != nullptr) {
    m_hueThumb->setFill(hsv(m_h, 1.0f, 1.0f));
    m_hueThumb->setBorder(rgba(1.0f, 1.0f, 1.0f, 1.0f), 2.0f * m_scale);
  }
}

void ColorPicker::applyPickFromSv(float localX, float localY) {
  if (m_svImage == nullptr) {
    return;
  }
  const float w = m_svImage->width();
  const float h = m_svImage->height();
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }
  m_s = std::clamp(localX / w, 0.0f, 1.0f);
  m_v = std::clamp(1.0f - localY / h, 0.0f, 1.0f);
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  syncFieldsFromColor();
  markPaintDirty();
  positionOverlays();
  if (m_onColorChanged) {
    m_onColorChanged(m_color);
  }
}

void ColorPicker::applyPickFromHue(float localX) {
  if (m_hueStrip == nullptr) {
    return;
  }
  const float w = m_hueStrip->width();
  if (w <= 0.0f) {
    return;
  }
  m_h = std::clamp(localX / w, 0.0f, 1.0f);
  m_svTextureDirty = true;
  m_color = hsv(m_h, m_s, m_v);
  m_color.a = m_alpha;
  syncFieldsFromColor();
  updateHueThumbStyle();
  markPaintDirty();
  markLayoutDirty();
  positionOverlays();
  if (m_onColorChanged) {
    m_onColorChanged(m_color);
  }
}

void ColorPicker::onHexInputChange(const std::string& value) {
  if (m_suppressFieldCallbacks) {
    return;
  }
  Color parsed{};
  if (!tryParseHexColor(value, parsed)) {
    return;
  }
  parsed.a = m_alpha;
  setColor(parsed);
  if (m_onColorChanged) {
    m_onColorChanged(m_color);
  }
}

void ColorPicker::onRgbInputChange() {
  if (m_suppressFieldCallbacks || m_rInput == nullptr || m_gInput == nullptr || m_bInput == nullptr) {
    return;
  }
  const int r = parseIntClamp(m_rInput->value(), 0, 255);
  const int g = parseIntClamp(m_gInput->value(), 0, 255);
  const int b = parseIntClamp(m_bInput->value(), 0, 255);
  Color c =
      rgba(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, m_alpha);
  setColor(c);
  if (m_onColorChanged) {
    m_onColorChanged(m_color);
  }
}

void ColorPicker::positionOverlays() {
  if (m_svImage != nullptr && m_svInput != nullptr) {
    m_svInput->setPosition(m_svImage->x(), m_svImage->y());
    m_svInput->setFrameSize(m_svImage->width(), m_svImage->height());
  }
  if (m_hueStrip != nullptr && m_hueInput != nullptr) {
    m_hueInput->setPosition(m_hueStrip->x(), m_hueStrip->y());
    m_hueInput->setFrameSize(m_hueStrip->width(), m_hueStrip->height());
  }
  if (m_svImage != nullptr && m_svThumb != nullptr) {
    const float w = m_svImage->width();
    const float h = m_svImage->height();
    const float cx = m_svImage->x() + m_s * w;
    const float cy = m_svImage->y() + (1.0f - m_v) * h;
    m_svThumb->setPosition(cx - m_svThumb->width() * 0.5f, cy - m_svThumb->height() * 0.5f);
  }
  if (m_hueStrip != nullptr && m_hueThumb != nullptr) {
    const float stripW = m_hueStrip->width();
    const float cx = m_hueStrip->x() + m_h * stripW;
    const float x =
        std::clamp(cx - m_hueThumb->width() * 0.5f, m_hueStrip->x(), m_hueStrip->x() + stripW - m_hueThumb->width());
    const float y = m_hueStrip->y() + (m_hueStrip->height() - m_hueThumb->height()) * 0.5f;
    m_hueThumb->setPosition(x, y);
  }
}

void ColorPicker::doLayout(Renderer& renderer) {
  const float pw = m_pickerWidth;
  const float stripH = std::max(10.0f, 12.0f * m_scale);

  if (m_svImage != nullptr) {
    m_svImage->setSize(pw, pw);
    m_svImage->layout(renderer);
    rebuildSvTexture(renderer);
  }
  if (m_hueStrip != nullptr) {
    m_hueStrip->setSize(pw, stripH);
    m_hueStrip->layout(renderer);
  }
  if (m_fieldsRow != nullptr) {
    m_fieldsRow->setSize(pw, 0.0f);
    m_fieldsRow->layout(renderer);
  }

  Flex::doLayout(renderer);
  positionOverlays();
}

LayoutSize ColorPicker::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void ColorPicker::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

ColorPickerSheet::ColorPickerSheet(float chromeScale) : m_chromeScale(std::max(0.1f, chromeScale)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd * m_chromeScale);
  setPadding(Style::spaceSm * m_chromeScale);

  addChild(
      ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * m_chromeScale,
          },
          ui::label({
              .out = &m_title,
              .text = i18n::tr("ui.dialogs.color-picker.title"),
              .fontSize = Style::fontSizeTitle * m_chromeScale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .fontWeight = FontWeight::Bold,
          }),
          ui::spacer(),
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
      )
  );

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
              .minWidth = 92.0f * m_chromeScale,
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
              .minWidth = 92.0f * m_chromeScale,
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

float ColorPickerSheet::preferredDialogWidth(float scale) { return 440.0f * std::max(0.1f, scale); }

float ColorPickerSheet::preferredDialogHeight(float dialogWidth, float scale) {
  const float s = std::max(0.1f, scale);
  const float pad = Style::spaceSm * s;
  const float innerW = std::max(160.0f, dialogWidth - 2.0f * pad);
  const float pickerColH = ColorPicker::intrinsicColumnHeight(innerW, s);
  const float titleH = Style::fontSizeTitle * s * 1.3f;
  const float actionsH = Style::controlHeight * s + 2.0f * Style::spaceSm * s;
  const float gapMd = Style::spaceMd * s;
  return 2.0f * pad + titleH + pickerColH + actionsH + 2.0f * gapMd;
}
