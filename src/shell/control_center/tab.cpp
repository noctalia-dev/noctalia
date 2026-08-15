#include "shell/control_center/tab.h"

#include "ui/builders.h"

#include <memory>

namespace control_center {

  void applySectionCardStyle(Flex& card, float scale, float fillOpacity) {
    card.setCardStyle(scale, fillOpacity);
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding(Style::spaceMd * scale);
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeTitle * scale,
        .fontWeight = FontWeight::Bold,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
    parent.addChild(std::move(label));
    return ptr;
  }

  void addBody(Flex& parent, const std::string& text, float scale) {
    parent.addChild(
        ui::label({
            .text = text,
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  std::unique_ptr<Flex> makeCardHeaderRow(const std::string& title, float scale) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .minHeight = Style::controlHeightSm * scale},
        ui::label({
            .text = title,
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
  }

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
