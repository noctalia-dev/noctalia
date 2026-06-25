#include "shell/bar/widgets/power_profile_widget.h"

#include "dbus/power/power_profiles_service.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>
#include <wayland-client-protocol.h>

PowerProfileWidget::PowerProfileWidget(PowerProfilesService* powerProfiles) : m_powerProfiles(powerProfiles) {}

void PowerProfileWidget::create() {
  auto area = std::make_unique<InputArea>();
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  // Left moves forward (toward performance), right moves backward (toward power-saver).
  area->setOnClick([this](const InputArea::PointerData& data) { cycleProfile(data.button == BTN_RIGHT ? -1 : 1); });
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    // Scroll up moves forward; Wayland reports up as a negative delta.
    cycleProfile(data.scrollDelta(1.0f) > 0 ? -1 : 1);
  });
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "balanced",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  setRoot(std::move(area));
}

void PowerProfileWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (m_glyph == nullptr || rootNode == nullptr) {
    return;
  }
  syncState(renderer);

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(
      m_available ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                  : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_glyph->measure(renderer);
  m_glyph->setPosition(0.0f, 0.0f);
  rootNode->setSize(m_glyph->width(), m_glyph->height());
}

void PowerProfileWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void PowerProfileWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr || m_area == nullptr) {
    return;
  }

  const std::string profile = m_powerProfiles != nullptr ? m_powerProfiles->activeProfile() : std::string{};
  const bool available = m_powerProfiles != nullptr && (!profile.empty() || !m_powerProfiles->profiles().empty());

  if (profile == m_lastProfile && available == m_available) {
    return;
  }

  m_available = available;
  m_lastProfile = profile;

  m_glyph->setGlyph(profileGlyphName(profile));
  m_glyph->setColor(
      m_available ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                  : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_glyph->measure(renderer);
  m_area->setEnabled(m_available);
  if (auto* rootNode = root(); rootNode != nullptr) {
    rootNode->setOpacity(m_available ? 1.0f : 0.55f);
  }
  requestRedraw();
}

void PowerProfileWidget::cycleProfile(int direction) {
  if (m_powerProfiles == nullptr) {
    return;
  }
  (void)m_powerProfiles->cycleActiveProfile(direction);
}
