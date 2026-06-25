#include "shell/switcher/window_switcher_tile.h"

#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>

namespace {

  constexpr float kIconHostAspect = 16.0f / 10.0f;
  constexpr float kIconScale = 0.5f;

  [[nodiscard]] float closeHitSize(float contentScale) { return Style::controlHeightSm * contentScale * 0.72f; }

  [[nodiscard]] float framePadding(float contentScale) { return Style::spaceXs * contentScale; }

  [[nodiscard]] float captionTextInset(float contentScale) { return Style::spaceSm * contentScale; }

  [[nodiscard]] float captionReserve(float contentScale) {
    const float innerGap = Style::spaceXs * contentScale;
    return Style::fontSizeCaption * contentScale * 1.3f + Style::fontSizeMini * contentScale * 1.15f + innerGap;
  }

  [[nodiscard]] std::pair<float, float>
  iconHostDimensions(float cellWidth, float cellHeight, float contentScale) noexcept {
    const float framePad = framePadding(contentScale);
    const float innerGap = Style::spaceXs * contentScale;
    const float iconHostWidth = std::max(0.0f, cellWidth - framePad * 2.0f);
    const float iconHostHeight = std::max(
        72.0f,
        std::min(
            iconHostWidth / kIconHostAspect,
            std::max(0.0f, cellHeight - framePad * 2.0f - innerGap - captionReserve(contentScale))
        )
    );
    return {iconHostWidth, iconHostHeight};
  }

  void applyCloseGlyphStyle(Glyph* glyph, const ColorSpec& fill, float contentScale) {
    if (glyph == nullptr) {
      return;
    }
    glyph->setColor(fill);
    const float offset = std::max(0.5f, 0.85f * contentScale);
    glyph->setShadow(Color{0.0f, 0.0f, 0.0f, 0.55f}, 0.0f, offset);
  }

} // namespace

bool WindowSwitcherTile::hitTestCloseRegion(
    float cellWidth, float cellHeight, float contentScale, float localX, float localY
) noexcept {
  const float framePad = framePadding(contentScale);
  const auto [iconHostWidth, iconHostHeight] = iconHostDimensions(cellWidth, cellHeight, contentScale);
  (void)iconHostHeight;

  const float hitSize = closeHitSize(contentScale);
  const float inset = Style::spaceXs * contentScale * 0.55f;
  const float x = framePad + iconHostWidth - hitSize - inset;
  const float y = framePad + inset;
  return localX >= x && localX < x + hitSize && localY >= y && localY < y + hitSize;
}

WindowSwitcherTile::WindowSwitcherTile(float contentScale, AsyncTextureCache* asyncTextures)
    : m_contentScale(contentScale), m_asyncTextures(asyncTextures) {
  setHitTestVisible(false);

  const float frameRadius = Style::scaledRadiusXl(m_contentScale);
  const float iconRadius = Style::scaledRadiusLg(m_contentScale);
  const float closeBackdropSize = closeHitSize(m_contentScale) + Style::spaceXs * m_contentScale * 0.45f;

  auto layout = ui::column({
      .out = &m_layout,
      .align = FlexAlign::Stretch,
  });
  addChild(std::move(layout));

  m_layout->addChild(
      ui::box({
          .out = &m_frame,
          .fill = colorSpecFromRole(ColorRole::Surface, 0.9f),
          .radius = frameRadius,
          .configure = [frameRadius](Box& box) {
            box.setRadius(frameRadius);
            box.setClipChildren(true);
          },
      })
  );

  m_frame->addChild(
      ui::column({
          .out = &m_inner,
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * m_contentScale,
          .configure = [this, iconRadius, closeBackdropSize](Flex& column) {
            column.addChild(
                ui::box({
                    .out = &m_iconHost,
                    .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
                    .radius = iconRadius,
                    .configure = [iconRadius](Box& box) {
                      box.setRadius(iconRadius);
                      box.setClipChildren(true);
                    },
                })
            );

            column.addChild(
                ui::column({
                    .out = &m_caption,
                    .align = FlexAlign::Stretch,
                    .justify = FlexJustify::Center,
                    .gap = Style::spaceXs * m_contentScale * 0.35f,
                    .flexGrow = 1.0f,
                    .configure = [this](Flex& caption) {
                      caption.addChild(
                          ui::label({
                              .out = &m_title,
                              .fontSize = Style::fontSizeCaption * m_contentScale,
                              .color = colorSpecFromRole(ColorRole::OnSurface),
                              .fontWeight = FontWeight::Bold,
                              .configure = [](Label& label) {
                                label.setMaxLines(1);
                                label.setTextAlign(TextAlign::Center);
                              },
                          })
                      );

                      caption.addChild(
                          ui::label({
                              .out = &m_subtitle,
                              .fontSize = Style::fontSizeMini * m_contentScale,
                              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                              .configure = [](Label& label) {
                                label.setMaxLines(1);
                                label.setTextAlign(TextAlign::Center);
                              },
                          })
                      );
                    },
                })
            );

            m_iconHost->addChild(
                ui::box({
                    .out = &m_closeBackdrop,
                    .fill = colorSpecFromRole(ColorRole::Surface, 0.92f),
                    .radius = closeBackdropSize * 0.5f,
                    .visible = false,
                    .participatesInLayout = false,
                })
            );

            m_iconHost->addChild(
                ui::glyph({
                    .out = &m_closeGlyph,
                    .glyph = "close",
                    .glyphSize = Style::fontSizeCaption * m_contentScale * 0.92f,
                    .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                    .participatesInLayout = false,
                })
            );
          },
      })
  );

  m_iconHost->addChild(
      ui::image({
          .out = &m_icon,
          .fit = ImageFit::Contain,
          .visible = false,
          .participatesInLayout = false,
      })
  );

  m_icon->setAsyncReadyCallback([this]() {
    if (m_icon == nullptr || m_fallbackGlyph == nullptr || !m_icon->hasImage()) {
      return;
    }
    m_icon->setVisible(true);
    m_fallbackGlyph->setVisible(false);
    markLayoutDirty();
    if (m_onInvalidate) {
      m_onInvalidate();
    }
  });

  m_iconHost->addChild(
      ui::glyph({
          .out = &m_fallbackGlyph,
          .glyph = "app-window",
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = true,
          .participatesInLayout = false,
      })
  );
}

void WindowSwitcherTile::setCellSize(float cellWidth, float cellHeight) {
  m_cellWidth = std::max(0.0f, cellWidth);
  m_cellHeight = std::max(0.0f, cellHeight);
  setSize(m_cellWidth, m_cellHeight);

  const float framePad = framePadding(m_contentScale);
  const float innerGap = Style::spaceXs * m_contentScale;
  std::tie(m_iconHostWidth, m_iconHostHeight) = iconHostDimensions(m_cellWidth, m_cellHeight, m_contentScale);

  if (m_layout != nullptr) {
    m_layout->setFrameSize(m_cellWidth, m_cellHeight);
  }
  if (m_frame != nullptr) {
    m_frame->setFrameSize(m_cellWidth, m_cellHeight);
  }
  if (m_inner != nullptr) {
    m_inner->setPadding(framePad);
    m_inner->setGap(innerGap);
    m_inner->setFrameSize(m_cellWidth, m_cellHeight);
  }
  if (m_iconHost != nullptr) {
    m_iconHost->setFrameSize(m_iconHostWidth, m_iconHostHeight);
  }
  const float captionTextWidth = std::max(0.0f, m_iconHostWidth - captionTextInset(m_contentScale) * 2.0f);
  if (m_title != nullptr) {
    m_title->setMaxWidth(captionTextWidth);
  }
  if (m_subtitle != nullptr) {
    m_subtitle->setMaxWidth(captionTextWidth);
  }

  markLayoutDirty();
}

void WindowSwitcherTile::setCloseHovered(bool hovered) {
  if (m_closeHovered == hovered) {
    return;
  }
  m_closeHovered = hovered;
  applyCloseVisualState();
}

void WindowSwitcherTile::bind(Renderer& renderer, const WindowSwitcherEntry& entry, bool selected, bool hovered) {
  m_entry = entry;
  m_hasEntry = true;
  m_selected = selected;
  m_hovered = hovered;

  const std::string title = entry.title.empty() ? entry.appLabel : entry.title;
  m_title->setText(title);

  const std::string subtitle = entry.appLabel.empty() ? entry.appId : entry.appLabel;
  const bool showSubtitle = !subtitle.empty() && StringUtils::toLower(subtitle) != StringUtils::toLower(title);
  m_subtitle->setVisible(showSubtitle);
  m_subtitle->setParticipatesInLayout(showSubtitle);
  if (showSubtitle) {
    m_subtitle->setText(subtitle);
  } else {
    m_subtitle->setText("");
  }

  if (entry.iconPath != m_iconPath) {
    m_iconPath = entry.iconPath;
    m_iconTargetSize = 0;
    m_icon->clear(renderer);
  }

  applyVisualState();
  applyCloseVisualState();
  markLayoutDirty();
}

bool WindowSwitcherTile::refreshIcon(Renderer& renderer) {
  if (!m_hasEntry || m_iconPath.empty()) {
    m_icon->setVisible(false);
    m_fallbackGlyph->setVisible(true);
    return false;
  }

  m_icon->setAppIconColorization(m_appIconColorizeTint);
  bool ready = false;
  if (m_asyncTextures != nullptr) {
    ready = m_icon->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
  } else {
    ready = m_icon->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
  }

  const float iconSize = std::min(m_iconHostWidth, m_iconHostHeight) * kIconScale;
  m_icon->setSize(iconSize, iconSize);
  m_icon->setVisible(ready);
  m_fallbackGlyph->setVisible(!ready);
  return ready;
}

void WindowSwitcherTile::applyVisualState() {
  const float frameRadius = Style::scaledRadiusXl(m_contentScale);
  if (m_selected) {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(colorSpecFromRole(ColorRole::Primary), Style::emphasizedBorderWidth);
    m_frame->setOpacity(1.0f);
  } else if (m_hovered) {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(colorSpecFromRole(ColorRole::Hover), Style::emphasizedBorderWidth);
    m_frame->setOpacity(1.0f);
  } else {
    m_frame->setFill(colorSpecFromRole(ColorRole::Surface));
    m_frame->setBorder(colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha), Style::borderWidth);
    m_frame->setOpacity(1.0f);
  }
  m_frame->setRadius(frameRadius);

  if (m_iconHost != nullptr) {
    m_iconHost->clearBorder();
  }
}

void WindowSwitcherTile::applyCloseVisualState() {
  if (m_closeGlyph == nullptr) {
    return;
  }
  if (m_closeBackdrop != nullptr) {
    m_closeBackdrop->setVisible(m_closeHovered);
  }
  if (m_closeHovered) {
    applyCloseGlyphStyle(m_closeGlyph, colorSpecFromRole(ColorRole::Error), m_contentScale);
  } else {
    applyCloseGlyphStyle(m_closeGlyph, colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.88f), m_contentScale);
  }
}

void WindowSwitcherTile::layoutOverlays(Renderer& renderer) {
  if (m_iconHostWidth <= 0.0f || m_iconHostHeight <= 0.0f) {
    return;
  }

  const float hitSize = closeHitSize(m_contentScale);
  const float inset = Style::spaceXs * m_contentScale * 0.55f;
  const float backdropSize = hitSize + Style::spaceXs * m_contentScale * 0.45f;
  const float closeX = std::round(m_iconHostWidth - hitSize - inset);
  const float closeY = std::round(inset);

  if (m_closeBackdrop != nullptr) {
    m_closeBackdrop->setPosition(
        std::round(closeX - (backdropSize - hitSize) * 0.5f), std::round(closeY - (backdropSize - hitSize) * 0.5f)
    );
    m_closeBackdrop->setSize(backdropSize, backdropSize);
  }
  if (m_closeGlyph != nullptr) {
    m_closeGlyph->measure(renderer);
    const float glyphW = m_closeGlyph->width() > 0.0f ? m_closeGlyph->width() : hitSize;
    const float glyphH = m_closeGlyph->height() > 0.0f ? m_closeGlyph->height() : hitSize;
    m_closeGlyph->setPosition(
        std::round(closeX + (hitSize - glyphW) * 0.5f), std::round(closeY + (hitSize - glyphH) * 0.5f)
    );
  }

  const int iconTarget =
      std::max(32, static_cast<int>(std::round(std::min(m_iconHostWidth, m_iconHostHeight) * kIconScale)));
  if (m_hasEntry && !m_iconPath.empty() && iconTarget != m_iconTargetSize) {
    m_iconTargetSize = iconTarget;
    (void)refreshIcon(renderer);
  } else if (m_hasEntry && !m_iconPath.empty()) {
    (void)refreshIcon(renderer);
  }

  const float iconSize = std::min(m_iconHostWidth, m_iconHostHeight) * kIconScale;
  if (m_icon != nullptr && m_icon->visible()) {
    m_icon->setPosition(
        std::round((m_iconHostWidth - m_icon->width()) * 0.5f), std::round((m_iconHostHeight - m_icon->height()) * 0.5f)
    );
  }
  if (m_fallbackGlyph != nullptr && m_fallbackGlyph->visible()) {
    m_fallbackGlyph->setGlyphSize(iconSize);
    m_fallbackGlyph->measure(renderer);
    m_fallbackGlyph->setPosition(
        std::round((m_iconHostWidth - m_fallbackGlyph->width()) * 0.5f),
        std::round((m_iconHostHeight - m_fallbackGlyph->height()) * 0.5f)
    );
  }
}

void WindowSwitcherTile::doLayout(Renderer& renderer) {
  if (m_cellWidth > 0.0f && m_cellHeight > 0.0f && (m_cellWidth != width() || m_cellHeight != height())) {
    setCellSize(width(), height());
  }

  InputArea::doLayout(renderer);
  layoutOverlays(renderer);
}
