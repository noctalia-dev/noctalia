#include "shell/settings/plugin_store_tile.h"

#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>

namespace settings {

  namespace {

    constexpr float kSourceBadgeMaxWidth = 120.0f;

  } // namespace

  PluginStoreTile::PluginStoreTile(float scale) : m_scale(scale) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setGap(Style::spaceXs * scale);
    setPadding(Style::spaceSm * scale);
    setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
    setRadius(Style::scaledRadiusMd(scale));

    addChild(
        ui::image({
            .out = &m_thumbnail,
            .fit = ImageFit::Cover,
            .radius = Style::scaledRadiusSm(scale),
            .width = -1.0f,
            .height = 80.0f * scale,
            .visible = false,
        })
    );

    addChild(
        ui::row(
            {.out = &m_iconContainer,
             .align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .height = 80.0f * scale},
            ui::glyph({
                .out = &m_icon,
                .glyph = "apps",
                .glyphSize = Style::fontSizeHeader * 1.5f * scale,
                .color = colorSpecFromRole(ColorRole::Primary),
            })
        )
    );

    addChild(
        ui::label({
            .out = &m_nameLabel,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Medium,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );

    auto titleRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});

    titleRow->addChild(
        ui::label({
            .out = &m_versionLabel,
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    titleRow->addChild(
        ui::row(
            {.out = &m_badge,
             .align = FlexAlign::Center,
             .paddingH = Style::spaceXs * scale,
             .fill = colorSpecFromRole(ColorRole::Primary, 0.15f),
             .radius = Style::scaledRadiusSm(scale),
             .maxWidth = kSourceBadgeMaxWidth * scale,
             .visible = false},
            ui::label({
                .out = &m_badgeLabel,
                .fontSize = Style::fontSizeMini * scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::Primary),
                .maxWidth = (kSourceBadgeMaxWidth - (Style::spaceXs * 2.0f)) * scale,
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            })
        )
    );

    titleRow->addChild(
        ui::row(
            {.out = &m_statusBadge,
             .align = FlexAlign::Center,
             .paddingH = Style::spaceXs * scale,
             .fill = colorSpecFromRole(ColorRole::Error, 0.15f),
             .radius = Style::scaledRadiusSm(scale),
             .maxWidth = kSourceBadgeMaxWidth * scale,
             .visible = false},
            ui::label({
                .out = &m_statusLabel,
                .fontSize = Style::fontSizeMini * scale,
                .fontWeight = FontWeight::Bold,
                .color = colorSpecFromRole(ColorRole::Error),
                .maxWidth = (kSourceBadgeMaxWidth - (Style::spaceXs * 2.0f)) * scale,
                .maxLines = 1,
                .ellipsize = TextEllipsize::End,
            })
        )
    );

    addChild(std::move(titleRow));

    addChild(
        ui::label({
            .out = &m_descLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 2,
            .ellipsize = TextEllipsize::End,
        })
    );

    addChild(ui::spacer());

    auto bottomRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true});
    bottomRow->addChild(
        ui::label({
            .out = &m_authorLabel,
            .fontSize = Style::fontSizeMini * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .flexGrow = 1.0f,
        })
    );
    bottomRow->addChild(
        ui::glyph({
            .out = &m_addedGlyph,
            .glyph = "check",
            .glyphSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
            .visible = false,
        })
    );
    addChild(std::move(bottomRow));
  }

  void PluginStoreTile::bind(
      const scripting::CatalogEntry& entry, std::string_view source, bool onDisk, bool selected, bool hovered,
      const std::string& thumbnailPath, Renderer* renderer, AsyncTextureCache* textureCache
  ) {
    // Thumbnail vs icon fallback.
    const bool hasThumbnail = !thumbnailPath.empty() && renderer != nullptr;
    if (hasThumbnail && thumbnailPath != m_boundThumbnailPath) {
      // The thumbnail is Cover-fit across the grid cell (~200px logical wide); decode to that
      // size and mipmap so the full-res webp does not alias when minified into the tile.
      const int targetSize = static_cast<int>(std::ceil(200.0f * m_scale));
      if (textureCache != nullptr) {
        m_thumbnail->setSourceFileAsync(*renderer, *textureCache, thumbnailPath, targetSize, true);
      } else {
        m_thumbnail->setSourceFile(*renderer, thumbnailPath, targetSize, true);
      }
      m_boundThumbnailPath = thumbnailPath;
    }
    m_thumbnail->setVisible(hasThumbnail);
    m_thumbnail->setParticipatesInLayout(hasThumbnail);
    m_iconContainer->setVisible(!hasThumbnail);
    m_iconContainer->setParticipatesInLayout(!hasThumbnail);
    if (!hasThumbnail) {
      m_icon->setGlyph(entry.icon.empty() ? std::string("apps") : entry.icon);
    }

    m_nameLabel->setText(entry.name);
    m_versionLabel->setText(entry.resolvedVersion.empty() ? std::string() : "v" + entry.resolvedVersion);

    if (source == "official") {
      m_badge->setVisible(true);
      m_badge->setParticipatesInLayout(true);
      m_badge->setFill(colorSpecFromRole(ColorRole::Primary, 0.15f));
      m_badgeLabel->setText(i18n::tr("settings.badges.official"));
      m_badgeLabel->setColor(colorSpecFromRole(ColorRole::Primary));
    } else if (source == "community") {
      m_badge->setVisible(true);
      m_badge->setParticipatesInLayout(true);
      m_badge->setFill(colorSpecFromRole(ColorRole::Secondary, 0.15f));
      m_badgeLabel->setText(i18n::tr("settings.badges.community"));
      m_badgeLabel->setColor(colorSpecFromRole(ColorRole::Secondary));
    } else {
      m_badge->setVisible(true);
      m_badge->setParticipatesInLayout(true);
      m_badge->setFill(colorSpecFromRole(ColorRole::Tertiary, 0.15f));
      m_badgeLabel->setText(source);
      m_badgeLabel->setColor(colorSpecFromRole(ColorRole::Tertiary));
    }

    // The grid is where a user first meets a plugin, so an unusable or older-than-latest
    // entry has to say so here rather than only in the detail sheet.
    const bool hasStatus = !entry.compatible || entry.heldBack;
    m_statusBadge->setVisible(hasStatus);
    m_statusBadge->setParticipatesInLayout(hasStatus);
    if (hasStatus) {
      const ColorRole role = entry.compatible ? ColorRole::Tertiary : ColorRole::Error;
      m_statusBadge->setFill(colorSpecFromRole(role, 0.15f));
      m_statusLabel->setColor(colorSpecFromRole(role));
      m_statusLabel->setText(
          i18n::tr(entry.compatible ? "settings.plugins.store.held-back" : "settings.plugins.store.incompatible")
      );
    }

    m_descLabel->setText(entry.description);
    m_authorLabel->setText(entry.author.empty() ? std::string() : entry.author);

    m_addedGlyph->setVisible(onDisk);
    m_addedGlyph->setParticipatesInLayout(onDisk);

    const ColorRole borderRole = selected ? ColorRole::Primary : (hovered ? ColorRole::Hover : ColorRole::Outline);
    setBorder(colorSpecFromRole(borderRole), selected ? Style::borderWidth * 2.0f : Style::borderWidth);
  }

} // namespace settings
