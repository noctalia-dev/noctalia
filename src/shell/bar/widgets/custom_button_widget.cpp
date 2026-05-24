#include "shell/bar/widgets/custom_button_widget.h"

#include "core/log.h"
#include "core/process.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

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

CustomButtonWidget::CustomButtonWidget(std::string glyph, std::string label, std::string tooltip, std::string imagePath,
                                       bool autoReloadImage, std::string command, std::string rightCommand,
                                       std::string middleCommand, std::string scrollUpCommand,
                                       std::string scrollDownCommand, FileWatcher* fileWatcher)
    : m_glyphName(std::move(glyph)), m_labelText(std::move(label)), m_tooltip(std::move(tooltip)),
      m_imagePath(std::move(imagePath)), m_autoReloadImage(autoReloadImage), m_command(std::move(command)),
      m_rightCommand(std::move(rightCommand)), m_middleCommand(std::move(middleCommand)),
      m_scrollUpCommand(std::move(scrollUpCommand)), m_scrollDownCommand(std::move(scrollDownCommand)),
      m_fileWatcher(fileWatcher) {}

CustomButtonWidget::~CustomButtonWidget() {
  if (m_fileWatcher != nullptr && m_imageWatchId != 0) {
    m_fileWatcher->unwatch(m_imageWatchId);
  }
}

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

  const bool hasScrollCommand = !m_scrollUpCommand.empty() || !m_scrollDownCommand.empty();
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
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return false;
    }

    const float delta = data.scrollDelta(1.0f);
    if (delta == 0.0f) {
      return false;
    }

    const std::string& command = delta < 0.0f ? m_scrollUpCommand : m_scrollDownCommand;
    if (command.empty()) {
      return false;
    }

    executeCommand(command);
    return true;
  });

  if (!m_tooltip.empty()) {
    area->setTooltip(m_tooltip);
  }

  auto glyph = std::make_unique<Glyph>();
  glyph->setGlyph(m_glyphName);
  glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
  glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  glyph->setVisible(!m_glyphName.empty());
  m_glyph = glyph.get();
  area->addChild(std::move(glyph));

  if (!m_imagePath.empty()) {
    auto image = std::make_unique<Image>();
    m_image = image.get();
    image->setFit(ImageFit::Contain);
    if (m_fileWatcher != nullptr) {
      m_imageWatchId = m_fileWatcher->watch(m_imagePath, [this]() {
        m_imageLoaded = false;
        requestRedraw();
      });
    }
    area->addChild(std::move(image));
  }

  auto label = std::make_unique<Label>();
  label->setText(m_labelText);
  label->setFontSize(Style::fontSizeBody * m_contentScale);
  label->setFontWeight(labelFontWeight());
  label->setMaxLines(1);
  label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  label->setVisible(!m_labelText.empty());
  m_label = label.get();
  area->addChild(std::move(label));

  m_area = area.get();
  setRoot(std::move(area));
}

bool CustomButtonWidget::reservesMiddleClick() const noexcept { return !m_middleCommand.empty(); }

void CustomButtonWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_area == nullptr || m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  const bool showImage = m_image != nullptr;
  const bool showGlyph = !showImage && !m_glyphName.empty();
  const bool showLabel = !m_labelText.empty();
  const float spacing = ((showGlyph || showImage) && showLabel) ? Style::spaceXs * m_contentScale : 0.0f;

  m_glyph->setVisible(showGlyph);
  m_label->setVisible(showLabel);

  if (showGlyph) {
    m_glyph->setGlyphSize(Style::barGlyphSize * m_contentScale);
    m_glyph->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
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

  if (showImage) {
    if (!m_imageLoaded) {
      auto fileData = FileUtils::readBinaryFile(m_imagePath);
      m_image->setSourceBytes(renderer, fileData.data(), fileData.size());
      m_imageLoaded = true;
      kLog.debug("Load image from '{}'", m_imagePath);
    }
    const float sizeLimit = Style::barIconSize * m_contentScale;
    const float aspectRatio = m_image->aspectRatio();

    if (isVertical) {
      m_image->setSize(sizeLimit, sizeLimit / aspectRatio);
    } else {
      m_image->setSize(sizeLimit * aspectRatio, sizeLimit);
    }
  }

  if (isVertical) {
    float width = 0.0f;
    float height = 0.0f;
    if (showImage) {
      width = std::max(width, m_image->width());
      height += m_image->height();
    }
    if (showGlyph) {
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
    if (showGlyph) {
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
  }
  if (showGlyph) {
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
  if (showGlyph) {
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
