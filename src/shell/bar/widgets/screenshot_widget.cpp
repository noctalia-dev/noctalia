#include "shell/bar/widgets/screenshot_widget.h"

#include "capture/screenshot_service.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  struct BarWidgetMenuAnchor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 1;
    std::int32_t h = 1;
    ContextMenuPopupPlacement placement{};
  };

  BarWidgetMenuAnchor barWidgetContextMenuAnchor(
      std::string_view barPosition, float widgetX, float widgetY, float widgetW, float widgetH, float contentScale
  ) {
    const float centerX = widgetX + (widgetW * 0.5f);
    const float centerY = widgetY + (widgetH * 0.5f);
    const std::int32_t gap = std::max(2, static_cast<std::int32_t>(std::lround(Style::spaceMd * contentScale)));
    const std::int32_t iconSize =
        std::clamp(static_cast<std::int32_t>(std::lround(std::max(widgetW, widgetH))), 16, 40);
    const std::int32_t halfIcon = iconSize / 2;

    BarWidgetMenuAnchor anchor{
        .x = static_cast<std::int32_t>(std::lround(centerX)) - halfIcon,
        .y = static_cast<std::int32_t>(std::lround(centerY)) - halfIcon,
        .w = iconSize,
        .h = iconSize,
    };

    if (barPosition == "bottom") {
      anchor.placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_TOP,
          .gravity = XDG_POSITIONER_GRAVITY_TOP,
          .offsetX = 0,
          .offsetY = -gap,
          .chromeAttachment = popup_chrome::Attachment{
              .horizontal = popup_chrome::HorizontalAttachment::Center,
              .vertical = popup_chrome::VerticalAttachment::Bottom,
          },
      };
    } else if (barPosition == "top") {
      anchor.placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
          .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
          .offsetX = 0,
          .offsetY = gap,
          .chromeAttachment = popup_chrome::Attachment{
              .horizontal = popup_chrome::HorizontalAttachment::Center,
              .vertical = popup_chrome::VerticalAttachment::Top,
          },
      };
    } else if (barPosition == "left") {
      anchor.placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_RIGHT,
          .gravity = XDG_POSITIONER_GRAVITY_RIGHT,
          .offsetX = gap,
          .offsetY = 0,
          .chromeAttachment = popup_chrome::Attachment{
              .horizontal = popup_chrome::HorizontalAttachment::Left,
              .vertical = popup_chrome::VerticalAttachment::Center,
          },
      };
    } else if (barPosition == "right") {
      anchor.placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_LEFT,
          .gravity = XDG_POSITIONER_GRAVITY_LEFT,
          .offsetX = -gap,
          .offsetY = 0,
          .chromeAttachment = popup_chrome::Attachment{
              .horizontal = popup_chrome::HorizontalAttachment::Right,
              .vertical = popup_chrome::VerticalAttachment::Center,
          },
      };
    } else {
      anchor.placement = ContextMenuPopupPlacement{
          .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
          .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
          .offsetX = 0,
          .offsetY = gap,
          .chromeAttachment = popup_chrome::Attachment{
              .horizontal = popup_chrome::HorizontalAttachment::Center,
              .vertical = popup_chrome::VerticalAttachment::Top,
          },
      };
    }

    return anchor;
  }

} // namespace

ScreenshotWidget::ScreenshotWidget(
    wl_output* output, std::string barGlyphId, ScreenshotService& screenshots, ConfigService& configService,
    CompositorPlatform& platform, RenderContext& renderContext, std::string barPosition, WidgetCustomImage customImage
)
    : m_barGlyphId(std::move(barGlyphId)), m_output(output), m_screenshots(screenshots), m_configService(configService),
      m_platform(platform), m_renderContext(renderContext), m_barPosition(std::move(barPosition)),
      m_customImage(std::move(customImage)) {}

ScreenshotWidget::~ScreenshotWidget() = default;

bool ScreenshotWidget::onPointerEvent(const PointerEvent& event) {
  if (m_menuPopup == nullptr || !m_menuPopup->isOpen()) {
    return false;
  }
  const bool consumed = m_menuPopup->onPointerEvent(event);
  if (!consumed && event.type == PointerEvent::Type::Button && event.pressed) {
    m_menuPopup->close();
    return true;
  }
  return consumed;
}

void ScreenshotWidget::create() {
  auto area = std::make_unique<InputArea>();
  m_hitArea = area.get();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.pressed) {
      return;
    }
    if (data.button == BTN_RIGHT) {
      openCaptureMenu();
      return;
    }
    if (data.button == BTN_LEFT) {
      runPrimaryClickAction();
    }
  });

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId.empty() ? "screenshot" : m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  setRoot(std::move(area));
}

void ScreenshotWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    if (auto* node = root(); node != nullptr) {
      node->setSize(m_image->width(), m_image->height());
    }
    return;
  }
  if (m_glyph == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  if (auto* node = root(); node != nullptr) {
    node->setSize(m_glyph->width(), m_glyph->height());
  }
}

ScreenshotService::OutputOptions ScreenshotWidget::outputOptions() const {
  return ScreenshotService::outputOptionsFromConfig(m_configService.config());
}

bool ScreenshotWidget::primaryClickIsFullscreen() const {
  if (configName().empty()) {
    return false;
  }
  const auto it = m_configService.config().widgets.find(std::string(configName()));
  if (it == m_configService.config().widgets.end()) {
    return false;
  }
  return it->second.getString("primary_click", "region") == "fullscreen";
}

void ScreenshotWidget::runPrimaryClickAction() {
  if (!m_screenshots.available()) {
    return;
  }
  const auto options = outputOptions();
  if (primaryClickIsFullscreen()) {
    m_screenshots.captureFullscreenInteractive(m_renderContext, options);
    return;
  }
  m_screenshots.beginRegionCapture(m_renderContext, options);
}

void ScreenshotWidget::openCaptureMenu() {
  if (!m_screenshots.available()) {
    return;
  }

  std::vector<ContextMenuControlEntry> entries;
  entries.push_back(
      ContextMenuControlEntry{
          .id = 1,
          .label = i18n::tr("bar.screenshot.fullscreen"),
          .enabled = true,
      }
  );
  entries.push_back(
      ContextMenuControlEntry{
          .id = 2,
          .label = i18n::tr("bar.screenshot.region"),
          .enabled = true,
      }
  );

  wl_surface* pointerSurface = m_platform.lastPointerSurface();
  auto* layerSurface = m_platform.layerSurfaceFor(pointerSurface);
  if (layerSurface == nullptr || m_hitArea == nullptr) {
    return;
  }

  if (m_menuPopup == nullptr) {
    m_menuPopup = std::make_unique<ContextMenuPopup>(m_platform.wayland(), m_renderContext);
  }
  m_menuPopup->setShadowConfig(m_configService.config().shell.shadow);
  const auto options = outputOptions();
  m_menuPopup->setOnActivate([this, options](const ContextMenuControlEntry& entry) {
    if (entry.id == 1) {
      m_screenshots.captureFullscreenInteractive(m_renderContext, options);
      return;
    }
    if (entry.id == 2) {
      m_screenshots.beginRegionCapture(m_renderContext, options);
      return;
    }
  });

  float absX = 0.0f;
  float absY = 0.0f;
  Node::absolutePosition(m_hitArea, absX, absY);
  const auto& area = *m_hitArea;
  const auto menuAnchor =
      barWidgetContextMenuAnchor(m_barPosition, absX, absY, area.width(), area.height(), m_contentScale);

  constexpr float kMenuWidth = 246.0f;
  const std::size_t maxVisible = std::max<std::size_t>(1, entries.size());
  m_menuPopup->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = kMenuWidth * m_contentScale,
          .maxMenuWidth = Style::menuAutoMaxWidth * m_contentScale,
          .maxVisible = maxVisible,
          .anchor =
              PopupAnchorRect{
                  .x = menuAnchor.x,
                  .y = menuAnchor.y,
                  .width = menuAnchor.w,
                  .height = menuAnchor.h,
              },
          .parent =
              PopupSurfaceParent{
                  .layerSurface = layerSurface,
                  .output = m_output,
                  .wlSurface = pointerSurface,
              },
          .placement = menuAnchor.placement,
      }
  );
}
