#include "shell/switcher/window_switcher_tile.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace {

  constexpr float kNearToneAlpha = Style::disabledOutlineAlpha * 0.25F;
  constexpr float kFarToneAlpha = Style::disabledOutlineAlpha * 0.5F;

} // namespace

WindowSwitcherTile::WindowSwitcherTile(float contentScale, AsyncTextureCache* asyncTextures)
    : m_contentScale(contentScale), m_asyncTextures(asyncTextures) {
  const float cardRadius = Style::scaledRadiusXl(m_contentScale);
  const float previewRadius = std::max(0.0F, cardRadius - Style::spaceXs * m_contentScale);

  setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
  setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  setOnClick([this](const InputArea::PointerData&) {
    if (m_onActivate) {
      m_onActivate();
    }
  });
  setOnEnter([this](const InputArea::PointerData&) { setPointerHovered(true); });
  setOnLeave([this]() { setPointerHovered(false); });

  addChild(
      ui::box({
          .out = &m_shadow,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  m_shadow->setZIndex(-1);

  addChild(
      ui::box({
          .out = &m_frame,
          .fill = colorSpecFromRole(ColorRole::Surface),
          .radius = cardRadius,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setClipChildren(true); },
      })
  );
  m_frame->addChild(
      ui::box({
          .out = &m_previewHost,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
          .radius = previewRadius,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setClipChildren(true); },
      })
  );
  m_previewHost->addChild(
      ui::image({
          .out = &m_thumbnail,
          .fit = ImageFit::Contain,
          .radius = previewRadius,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  m_previewHost->addChild(
      ui::box({
          .out = &m_toneOverlay,
          .fill = clearColorSpec(),
          .radius = previewRadius,
          .participatesInLayout = false,
      })
  );
  m_previewHost->addChild(
      ui::image({
          .out = &m_icon,
          .fit = ImageFit::Contain,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  m_previewHost->addChild(
      ui::glyph({
          .out = &m_fallbackGlyph,
          .glyph = "app-window",
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .participatesInLayout = false,
      })
  );
  m_previewHost->addChild(
      ui::button({
          .out = &m_close,
          .glyph = "close",
          .glyphSize = Style::fontSizeCaption * m_contentScale,
          .controlHeight = (Style::controlHeightSm - Style::spaceSm) * m_contentScale,
          .variant = ButtonVariant::Outline,
          .padding = 0.0F,
          .width = (Style::controlHeightSm - Style::spaceSm) * m_contentScale,
          .height = (Style::controlHeightSm - Style::spaceSm) * m_contentScale,
          .participatesInLayout = false,
          .onClick = [this]() {
            if (m_onClose) {
              m_onClose();
            }
          },
      })
  );
  const float closeHitSlop = Style::spaceXs * m_contentScale;
  m_close->inputArea()->setHitTestOutset(
      HitTestOutset{
          .left = closeHitSlop,
          .top = closeHitSlop,
          .right = closeHitSlop,
          .bottom = closeHitSlop,
      }
  );
  m_close->setOnEnter([this]() { setPointerHovered(true); });
  m_close->setOnLeave([this]() { setPointerHovered(false); });

  auto captionBadge = ui::box({
      .out = &m_captionBadge,
      .fill = colorSpecFromRole(ColorRole::Surface),
      .border = scaleAlpha(colorSpecFromRole(ColorRole::Outline), Style::disabledOutlineAlpha),
      .borderWidth = Style::borderWidth,
      .radius = cardRadius,
      .visible = false,
      .participatesInLayout = false,
  });
  captionBadge->addChild(
      ui::column(
          {
              .out = &m_caption,
              .align = FlexAlign::Stretch,
              .justify = FlexJustify::Center,
              .gap = Style::windowSwitcherCaptionLineGap * m_contentScale,
              .participatesInLayout = false,
          },
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * m_contentScale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .textAlign = TextAlign::Center,
              .ellipsize = TextEllipsize::End,
          }),
          ui::label({
              .out = &m_subtitle,
              .fontSize = Style::fontSizeMini * m_contentScale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .textAlign = TextAlign::Center,
              .ellipsize = TextEllipsize::End,
          })
      )
  );
  captionBadge->setZIndex(2);
  addChild(std::move(captionBadge));

  m_icon->setAsyncReadyCallback([this]() {
    if (!m_showAppIcon || m_icon == nullptr || m_fallbackGlyph == nullptr || !m_icon->hasImage()) {
      return;
    }
    m_icon->setVisible(true);
    m_fallbackGlyph->setVisible(false);
    markLayoutDirty();
    if (m_onInvalidate) {
      m_onInvalidate();
    }
  });
}

void WindowSwitcherTile::setCardSize(float width, float height) {
  m_cardWidth = std::max(0.0F, width);
  m_cardHeight = std::max(0.0F, height);
  setSize(m_cardWidth, m_cardHeight);
  markLayoutDirty();
}

void WindowSwitcherTile::setShadowStyle(const RoundedRectStyle& style) {
  m_shadowStyle = style;
  m_shadowConfigured = true;
  if (m_shadow != nullptr) {
    m_shadow->setStyle(style);
  }
  applyVisualState();
}

void WindowSwitcherTile::setShowCaption(bool show) {
  m_showCaption = show;
  applyVisualState();
  markLayoutDirty();
}

void WindowSwitcherTile::setShowAppIcon(bool show) {
  m_showAppIcon = show;
  if (!show) {
    m_icon->setVisible(false);
    m_fallbackGlyph->setVisible(false);
  }
  markLayoutDirty();
}

void WindowSwitcherTile::bind(
    Renderer& renderer, const WindowSwitcherEntry& entry, WindowSwitcherTileDepth depth, bool showCaption,
    bool wideCaption, WindowSwitcherHorizontalPlacement iconPlacement, WindowSwitcherHorizontalPlacement closePlacement
) {
  m_hasEntry = true;
  m_depth = depth;
  m_selected = depth == WindowSwitcherTileDepth::Selected;
  m_captionVisible = showCaption;
  m_wideCaption = wideCaption;
  m_iconPlacement = iconPlacement;
  m_closePlacement = closePlacement;
  m_title->setText(entry.title.empty() ? entry.appLabel : entry.title);
  m_subtitle->setText(entry.appLabel.empty() ? entry.appId : entry.appLabel);

  if (entry.iconPath != m_iconPath) {
    m_iconPath = entry.iconPath;
    m_iconTargetSize = 0;
    m_icon->clear(renderer);
  }
  if (entry.thumbnail != m_thumbnailImage) {
    m_thumbnailImage = entry.thumbnail;
    m_thumbnail->clear(renderer);
    bool loaded = false;
    if (m_thumbnailImage != nullptr
        && m_thumbnailImage->width > 0
        && m_thumbnailImage->height > 0
        && !m_thumbnailImage->rgba.empty()) {
      loaded = m_thumbnail->setSourceRaw(
          renderer, m_thumbnailImage->rgba.data(), m_thumbnailImage->rgba.size(), m_thumbnailImage->width,
          m_thumbnailImage->height, m_thumbnailImage->width * 4, PixmapFormat::RGBA, true
      );
    }
    m_thumbnail->setVisible(loaded);
  }
  applyVisualState();
  markLayoutDirty();
}

void WindowSwitcherTile::setPointerHovered(bool hovered) {
  if (m_pointerHovered == hovered) {
    return;
  }
  m_pointerHovered = hovered;
  applyVisualState();

  if (m_onInvalidate) {
    m_onInvalidate();
  }
}

bool WindowSwitcherTile::refreshIcon(Renderer& renderer) {
  if (!m_showAppIcon) {
    m_icon->setVisible(false);
    m_fallbackGlyph->setVisible(false);
    return false;
  }
  if (!m_hasEntry || m_iconPath.empty()) {
    m_icon->setVisible(false);
    m_fallbackGlyph->setVisible(true);
    return false;
  }
  m_icon->setAppIconColorization(m_appIconColorizeTint);
  const bool ready = m_asyncTextures != nullptr
      ? m_icon->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true)
      : m_icon->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
  m_icon->setVisible(ready);
  m_fallbackGlyph->setVisible(!ready);
  return ready;
}

void WindowSwitcherTile::applyVisualState() {
  if (m_selected) {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(colorSpecFromRole(ColorRole::Primary), Style::emphasizedBorderWidth);
    m_previewHost->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
  } else if (m_pointerHovered) {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(colorSpecFromRole(ColorRole::Hover), Style::emphasizedBorderWidth);
    m_previewHost->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
  } else {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(
        scaleAlpha(colorSpecFromRole(ColorRole::Outline), Style::disabledOutlineAlpha), Style::borderWidth
    );
    m_previewHost->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
  }
  if (m_toneOverlay != nullptr) {
    const float tone = m_depth == WindowSwitcherTileDepth::Far
        ? kFarToneAlpha
        : (m_depth == WindowSwitcherTileDepth::Near ? kNearToneAlpha : 0.0F);
    m_toneOverlay->setFill(scaleAlpha(colorSpecFromRole(ColorRole::Shadow), tone));
  }
  if (m_captionBadge != nullptr) {
    m_captionBadge->setVisible(m_captionVisible && m_showCaption);
  }
  if (m_shadow != nullptr) {
    m_shadow->setVisible(m_selected && m_shadowConfigured);
  }
  const bool showClose = m_pointerHovered;
  m_close->setVisible(showClose);
  m_close->setEnabled(showClose);
}

void WindowSwitcherTile::layoutContent(Renderer& renderer) {
  const float outerPad = Style::spaceXs * m_contentScale;
  const float innerW = std::max(0.0F, m_cardWidth - outerPad * 2.0F);
  const float captionH = m_captionVisible && m_showCaption
      ? std::min(Style::windowSwitcherCaptionHeight * m_contentScale, m_cardHeight)
      : 0.0F;
  const float captionGap = captionH > 0.0F ? Style::spaceMd * m_contentScale : 0.0F;
  const float frameH = std::max(0.0F, m_cardHeight - captionGap - captionH);
  const float previewH = std::max(0.0F, frameH - outerPad * 2.0F);

  if (m_shadow != nullptr) {
    m_shadow->setPosition(m_shadowStyle.shadowCutoutOffsetX, m_shadowStyle.shadowCutoutOffsetY);
    m_shadow->setFrameSize(m_cardWidth, frameH);
  }
  m_frame->setPosition(0.0F, 0.0F);
  m_frame->setFrameSize(m_cardWidth, frameH);
  m_previewHost->setPosition(outerPad, outerPad);
  m_previewHost->setFrameSize(innerW, previewH);
  m_thumbnail->setPosition(0.0F, 0.0F);
  m_thumbnail->setSize(innerW, previewH);
  m_toneOverlay->setPosition(0.0F, 0.0F);
  m_toneOverlay->setFrameSize(innerW, previewH);

  const bool hasThumbnail = m_thumbnail->visible();
  const float iconScale = hasThumbnail ? Style::windowSwitcherPreviewIconScale : Style::windowSwitcherFallbackIconScale;
  const float iconSize = std::min(innerW, previewH) * iconScale;
  const int targetSize = std::max(
      static_cast<int>(std::round(Style::baseGlyphSize * m_contentScale)), static_cast<int>(std::round(iconSize))
  );
  if (m_showAppIcon && targetSize != m_iconTargetSize) {
    m_iconTargetSize = targetSize;
    (void)refreshIcon(renderer);
  } else if (m_showAppIcon && m_hasEntry && !m_iconPath.empty()) {
    (void)refreshIcon(renderer);
  } else if (!m_showAppIcon) {
    m_icon->setVisible(false);
    m_fallbackGlyph->setVisible(false);
  }
  const float overlayInset = Style::spaceSm * m_contentScale;
  float iconX = (innerW - iconSize) * 0.5F;
  if (hasThumbnail && m_iconPlacement == WindowSwitcherHorizontalPlacement::Left) {
    iconX = overlayInset;
  } else if (hasThumbnail && m_iconPlacement == WindowSwitcherHorizontalPlacement::Right) {
    iconX = innerW - iconSize - overlayInset;
  }
  const float iconY = hasThumbnail ? previewH - iconSize - overlayInset : (previewH - iconSize) * 0.5F;
  m_icon->setSize(iconSize, iconSize);
  m_icon->setPosition(std::round(iconX), std::round(iconY));
  m_fallbackGlyph->setGlyphSize(iconSize);
  m_fallbackGlyph->measure(renderer);
  m_fallbackGlyph->setPosition(
      std::round(iconX + (iconSize - m_fallbackGlyph->width()) * 0.5F),
      std::round(iconY + (iconSize - m_fallbackGlyph->height()) * 0.5F)
  );

  const float closeSize = (Style::controlHeightSm - Style::spaceSm) * m_contentScale;
  const float closeInset = Style::spaceXs * m_contentScale;
  const float closeX =
      m_closePlacement == WindowSwitcherHorizontalPlacement::Left ? closeInset : innerW - closeSize - closeInset;
  m_close->setPosition(std::round(closeX), std::round(closeInset));
  m_close->setSize(closeSize, closeSize);

  if (m_captionBadge != nullptr && m_captionVisible && m_showCaption) {
    const float captionPad = Style::spaceLg * m_contentScale;
    const float captionMargin = m_wideCaption ? 0.0F : Style::spaceLg * m_contentScale;
    const float maxCaptionW = std::max(0.0F, m_cardWidth - captionMargin * 2.0F);
    m_title->setMaxWidth(std::max(0.0F, maxCaptionW - captionPad * 2.0F));
    m_subtitle->setMaxWidth(std::max(0.0F, maxCaptionW - captionPad * 2.0F));
    m_title->measure(renderer);
    m_subtitle->measure(renderer);
    const float contentCaptionW = std::max(m_title->width(), m_subtitle->width()) + captionPad * 2.0F;
    const float captionW = m_wideCaption ? maxCaptionW : std::min(maxCaptionW, contentCaptionW);
    const float captionX = (m_cardWidth - captionW) * 0.5F;
    const float captionY = frameH + captionGap;
    m_captionBadge->setPosition(captionX, captionY);
    m_captionBadge->setFrameSize(captionW, captionH);
    m_caption->setPosition(captionPad, 0.0F);
    m_caption->setFrameSize(std::max(0.0F, captionW - captionPad * 2.0F), captionH);
  }
}

void WindowSwitcherTile::doLayout(Renderer& renderer) {
  layoutContent(renderer);
  InputArea::doLayout(renderer);
}
