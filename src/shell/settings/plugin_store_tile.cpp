#include "shell/settings/plugin_store_tile.h"

#include "i18n/i18n.h"
#include "render/core/async_texture_cache.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace settings {

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
            .height = 100.0f * scale,
            .visible = false,
        })
    );

    addChild(
        ui::row(
            {.out = &m_iconContainer,
             .align = FlexAlign::Center,
             .justify = FlexJustify::Center,
             .height = 100.0f * scale},
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
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .fontWeight = FontWeight::Medium,
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
             .visible = false},
            ui::label({
                .out = &m_badgeLabel,
                .fontSize = Style::fontSizeMini * scale,
                .color = colorSpecFromRole(ColorRole::Primary),
                .fontWeight = FontWeight::Bold,
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
      const scripting::CatalogEntry& entry, std::string_view source, bool onDisk, bool hovered,
      const std::string& thumbnailPath, Renderer* renderer, AsyncTextureCache* textureCache
  ) {
    // Thumbnail vs icon fallback.
    const bool hasThumbnail = !thumbnailPath.empty() && renderer != nullptr;
    if (hasThumbnail && thumbnailPath != m_boundThumbnailPath) {
      if (textureCache != nullptr) {
        m_thumbnail->setSourceFileAsync(*renderer, *textureCache, thumbnailPath);
      } else {
        m_thumbnail->setSourceFile(*renderer, thumbnailPath);
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
    m_versionLabel->setText(entry.version.empty() ? std::string() : "v" + entry.version);

    if (source == "official") {
      m_badge->setVisible(true);
      m_badge->setParticipatesInLayout(true);
      m_badgeLabel->setText(i18n::tr("settings.badges.official"));
    } else if (source == "community") {
      m_badge->setVisible(true);
      m_badge->setParticipatesInLayout(true);
      m_badgeLabel->setText(i18n::tr("settings.badges.community"));
    } else {
      m_badge->setVisible(false);
      m_badge->setParticipatesInLayout(false);
    }

    m_descLabel->setText(entry.description);
    m_authorLabel->setText(entry.author.empty() ? std::string() : entry.author);

    m_addedGlyph->setVisible(onDisk);
    m_addedGlyph->setParticipatesInLayout(onDisk);

    setBorder(colorSpecFromRole(hovered ? ColorRole::Hover : ColorRole::Outline), Style::borderWidth);
  }

} // namespace settings
