#include "shell/settings/template_store_tile.h"

#include "ui/builders.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace settings {

  TemplateStoreTile::TemplateStoreTile(float scale) : m_scale(scale) {
    // Match built-in template cards in settings_content makeTemplateGridBlock.
    setDirection(FlexDirection::Horizontal);
    setAlign(FlexAlign::Center);
    setGap(Style::spaceXs * scale);
    setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
    setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F));
    setBorder(colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha), Style::borderWidth * scale);
    setRadius(Style::scaledRadiusMd(scale));

    addChild(
        ui::checkbox({
            .out = &m_checkbox,
            .scale = scale,
            .checkedFill = colorSpecFromRole(ColorRole::Surface),
            .checkedBorder = colorSpecFromRole(ColorRole::OnPrimary),
            .checkedGlyph = colorSpecFromRole(ColorRole::Primary),
        })
    );

    auto text = ui::column({.align = FlexAlign::Start, .flexGrow = 1.0F});
    text->addChild(
        ui::label({
            .out = &m_nameLabel,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Medium,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    text->addChild(
        ui::label({
            .out = &m_categoryLabel,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .visible = false,
        })
    );
    addChild(std::move(text));
  }

  void TemplateStoreTile::bind(
      std::string_view name, std::string_view category, bool enabled, bool selected, bool hovered,
      std::function<void(bool enabled)> onToggle
  ) {
    m_nameLabel->setText(std::string(name));
    m_categoryLabel->setText(std::string(category));

    const bool hasCategory = !category.empty();
    m_categoryLabel->setVisible(hasCategory);
    m_categoryLabel->setParticipatesInLayout(hasCategory);

    if (m_checkbox != nullptr) {
      m_checkbox->setOnChange(nullptr);
      m_checkbox->setChecked(enabled);
      m_checkbox->setCheckedColors(
          colorSpecFromRole(ColorRole::Surface), colorSpecFromRole(ColorRole::OnPrimary),
          colorSpecFromRole(ColorRole::Primary)
      );
      m_checkbox->setOnChange([onToggle = std::move(onToggle)](bool next) {
        if (onToggle) {
          onToggle(next);
        }
      });
    }

    // Same palette as built-in template Button::ButtonPalette (normal / hover).
    const float borderWidth = Style::borderWidth * m_scale;
    if (enabled) {
      setFill(colorSpecFromRole(ColorRole::Primary));
      setBorder(colorSpecFromRole(ColorRole::Primary, 0.9F), borderWidth);
      m_nameLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      m_categoryLabel->setColor(colorSpecFromRole(ColorRole::OnPrimary, 0.75F));
    } else if (hovered || selected) {
      setFill(colorSpecFromRole(ColorRole::Hover));
      setBorder(colorSpecFromRole(ColorRole::Hover), borderWidth);
      m_nameLabel->setColor(colorSpecFromRole(ColorRole::OnHover));
      m_categoryLabel->setColor(colorSpecFromRole(ColorRole::OnHover, 0.75F));
    } else {
      setFill(colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F));
      setBorder(colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha), borderWidth);
      m_nameLabel->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_categoryLabel->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    }
  }

} // namespace settings
