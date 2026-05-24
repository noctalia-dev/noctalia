#include "shell/control_center/tab.h"

#include "ui/builders.h"

#include <memory>

namespace control_center {

  void applySectionCardStyle(Flex& card, float scale, float fillOpacity, bool showBorder) {
    card.setCardStyle(scale, fillOpacity, showBorder);
    card.setDirection(FlexDirection::Vertical);
    card.setAlign(FlexAlign::Stretch);
    card.setGap(Style::spaceSm * scale);
    card.setPadding((Style::spaceSm + Style::spaceXs) * scale, Style::spaceMd * scale);
  }

  Label* addTitle(Flex& parent, const std::string& text, float scale) {
    Label* ptr = nullptr;
    auto label = ui::label({
        .out = &ptr,
        .text = text,
        .fontSize = Style::fontSizeTitle * scale,
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .fontWeight = FontWeight::Bold,
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

} // namespace control_center

std::unique_ptr<Flex> Tab::createHeaderActions() { return nullptr; }
