#include "shell/bar/widgets/custom_button_widget.h"

#include "core/log.h"
#include "core/process/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <linux/input-event-codes.h>
#include <memory>
#include <utility>
#include <wayland-client-protocol.h>

namespace {
  constexpr Logger kLog("custom-button");
}

CustomButtonWidget::CustomButtonWidget(Options options)
    : m_glyphName(std::move(options.glyph)), m_labelText(std::move(options.label)),
      m_tooltip(std::move(options.tooltip)), m_command(std::move(options.command)),
      m_rightCommand(std::move(options.rightCommand)), m_middleCommand(std::move(options.middleCommand)),
      m_scrollUpCommand(std::move(options.scrollUpCommand)), m_scrollDownCommand(std::move(options.scrollDownCommand)),
      m_enableScroll(options.enableScroll), m_customImage(std::move(options.customImage)) {}

void CustomButtonWidget::create() {
  auto area = std::make_unique<InputArea>();

  std::uint32_t acceptedButtons = 0;
  if (!m_command.empty()) {
    acceptedButtons |= InputArea::buttonMask(BTN_LEFT);
  }
  if (!m_rightCommand.empty()) {
    acceptedButtons |= InputArea::buttonMask(BTN_RIGHT);
  }
  if (!m_middleCommand.empty()) {
    acceptedButtons |= InputArea::buttonMask(BTN_MIDDLE);
  }
  area->setAcceptedButtons(acceptedButtons);

  const bool hasScrollCommand = m_enableScroll && (!m_scrollUpCommand.empty() || !m_scrollDownCommand.empty());
  if (acceptedButtons != 0 || hasScrollCommand) {
    area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  }
  if (acceptedButtons != 0) {
    area->setOnClick([this](const InputArea::PointerData& data) {
      switch (data.button) {
      case BTN_LEFT:
        executeCommand(m_command);
        break;
      case BTN_RIGHT:
        executeCommand(m_rightCommand);
        break;
      case BTN_MIDDLE:
        executeCommand(m_middleCommand);
        break;
      default:
        break;
      }
    });
  }

  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (!m_enableScroll || data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }

    const float steps = data.scrollSteps();
    if (steps == 0.0f) {
      return false;
    }

    const std::string& command = steps < 0.0f ? m_scrollUpCommand : m_scrollDownCommand;
    if (command.empty()) {
      return false;
    }

    executeCommand(command);
    return true;
  });

  if (!m_tooltip.empty()) {
    area->setTooltip(m_tooltip);
  }

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_glyphName,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
            .visible = !m_glyphName.empty(),
        })
    );
  }

  area->addChild(
      ui::label({
          .out = &m_label,
          .text = m_labelText,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxLines = 1,
          .visible = !m_labelText.empty(),
      })
  );

  m_area = area.get();
  setRoot(std::move(area));
}

bool CustomButtonWidget::reservesMiddleClick(float /*sceneX*/, float /*sceneY*/) const noexcept {
  return !m_middleCommand.empty();
}

void CustomButtonWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_area == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  const bool showImage = m_image != nullptr;
  const bool showGlyph = !showImage && m_glyph != nullptr && !m_glyphName.empty();
  const bool showIcon = showImage || showGlyph;
  const bool showLabel = !m_labelText.empty();
  const float spacing = (showIcon && showLabel) ? Style::spaceXs * m_contentScale : 0.0f;

  if (m_glyph != nullptr) {
    m_glyph->setVisible(showGlyph);
  }
  m_label->setVisible(showLabel);

  if (showImage) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
  } else if (showGlyph) {
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
  }

  if (showLabel) {
    m_label->setFontSize((isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * m_contentScale);
    m_label->setFontWeight(labelFontWeight());
    m_label->setTextAlign(isVertical ? TextAlign::Center : TextAlign::Start);
    m_label->setMaxWidth(isVertical ? containerWidth : 0.0f);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  if (isVertical) {
    float width = 0.0f;
    float height = 0.0f;
    if (showImage) {
      width = std::max(width, m_image->width());
      height += m_image->height();
    } else if (showGlyph) {
      width = std::max(width, m_glyph->width());
      height += m_glyph->height();
    }
    if (showLabel) {
      if (height > 0.0f) {
        height += spacing;
      }
      width = std::max(width, m_label->width());
      height += m_label->height();
    }

    float y = 0.0f;
    if (showImage) {
      m_image->setPosition(std::round((width - m_image->width()) * 0.5f), y);
      y += m_image->height() + spacing;
    } else if (showGlyph) {
      m_glyph->setPosition(std::round((width - m_glyph->width()) * 0.5f), y);
      y += m_glyph->height() + spacing;
    }
    if (showLabel) {
      m_label->setPosition(std::round((width - m_label->width()) * 0.5f), y);
    }
    m_area->setSize(width, height);
    return;
  }

  float width = 0.0f;
  float height = 0.0f;
  if (showImage) {
    width += m_image->width();
    height = std::max(height, m_image->height());
  } else if (showGlyph) {
    width += m_glyph->width();
    height = std::max(height, m_glyph->height());
  }
  if (showLabel) {
    if (width > 0.0f) {
      width += spacing;
    }
    width += m_label->width();
    height = std::max(height, m_label->height());
  }

  float x = 0.0f;
  if (showImage) {
    m_image->setPosition(x, std::round((height - m_image->height()) * 0.5f));
    x += m_image->width() + spacing;
  } else if (showGlyph) {
    m_glyph->setPosition(x, std::round((height - m_glyph->height()) * 0.5f));
    x += m_glyph->width() + spacing;
  }
  if (showLabel) {
    m_label->setPosition(x, std::round((height - m_label->height()) * 0.5f));
  }
  m_area->setSize(width, height);
}

void CustomButtonWidget::executeCommand(const std::string& command) const {
  if (command.empty()) {
    return;
  }
  if (!process::runAsync(command)) {
    kLog.warn("failed to launch command for '{}'", configName());
  }
}
