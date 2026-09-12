#include "shell/settings/display/revert_dialog_modal.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/settings/display/display_service.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/style.h"

#include <algorithm>

namespace settings::display {

  namespace {

    constexpr float kDialogWidth = 420.0F;
    constexpr int kRevertTimeoutSeconds = 15;

  } // namespace

  void RevertDialogModal::open(SettingsModalHost& host, DisplayService& display, float scale) {
    if (isOpen()) {
      return;
    }
    m_display = &display;
    m_scale = std::max(0.1F, scale);
    m_lastCountdown = -1;
    pushModal(host, Style::spaceMd * m_scale, 24.0F * m_scale);
    if (!isOpen()) {
      m_display = nullptr;
      resetRoot();
    }
  }

  void RevertDialogModal::updateContent(Renderer& /*renderer*/) {
    if (!isOpen() || m_display == nullptr) {
      return;
    }
    if (!m_display->awaitingConfirmation()) {
      close();
      return;
    }
    if (m_lastCountdown != m_display->revertCountdown()) {
      m_lastCountdown = m_display->revertCountdown();
      rebuild();
    }
  }

  void RevertDialogModal::onClosed() {
    m_display = nullptr;
    resetRoot();
  }

  std::unique_ptr<Node> RevertDialogModal::buildContent() {
    const float gap = Style::spaceMd * m_scale;
    auto root = ui::column({
        .out = &m_root,
        .align = FlexAlign::Stretch,
        .gap = gap,
        .padding = Style::spaceMd * m_scale,
        .fillWidth = true,
    });

    const int countdown = m_display != nullptr ? std::max(0, m_display->revertCountdown()) : 0;
    root->addChild(
        ui::label({
            .text = i18n::tr("settings.display.keep-changes"),
            .fontSize = Style::fontSizeTitle * m_scale,
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
        })
    );
    root->addChild(
        ui::label({
            .text = i18n::tr("settings.display.reverting-in-seconds", "seconds", countdown),
            .fontSize = Style::fontSizeBody * m_scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );

    const float barWidth = (kDialogWidth - 4.0F * Style::spaceMd) * m_scale;
    const float fraction = static_cast<float>(countdown) / static_cast<float>(kRevertTimeoutSeconds);
    auto bar = ui::column({
        .align = FlexAlign::Stretch,
        .fillWidth = true,
    });
    auto barFill = ui::box({
        .fill = colorSpecFromRole(ColorRole::Error),
        .radius = 3.0F * m_scale,
    });
    barFill->setParticipatesInLayout(false);
    barFill->setSize(barWidth * fraction, 6.0F * m_scale);
    bar->addChild(std::move(barFill));
    bar->setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
    bar->setRadius(3.0F * m_scale);
    bar->setSize(barWidth, 6.0F * m_scale);
    root->addChild(std::move(bar));

    root->addChild(
        ui::row(
            {
                .align = FlexAlign::Center,
                .justify = FlexJustify::Center,
                .gap = Style::spaceMd * m_scale,
            },
            ui::button({
                .text = i18n::tr("common.revert"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick =
                    [this]() {
                      const auto alive = aliveToken();
                      DeferredCall::callLater([this, alive]() {
                        if (alive.expired()) {
                          return;
                        }
                        if (m_display != nullptr) {
                          m_display->revertChanges();
                        }
                        close();
                      });
                    },
            }),
            ui::button({
                .text = i18n::tr("common.keep"),
                .fontSize = Style::fontSizeBody * m_scale,
                .variant = ButtonVariant::Primary,
                .minHeight = Style::controlHeight * m_scale,
                .paddingV = Style::spaceXs * m_scale,
                .paddingH = Style::spaceMd * m_scale,
                .radius = Style::scaledRadiusMd(m_scale),
                .onClick = [this]() {
                  const auto alive = aliveToken();
                  DeferredCall::callLater([this, alive]() {
                    if (alive.expired()) {
                      return;
                    }
                    if (m_display != nullptr) {
                      m_display->keepChanges();
                    }
                    close();
                  });
                },
            })
        )
    );

    return root;
  }

  LayoutSize RevertDialogModal::measureContent(Renderer& renderer, const SettingsModalLayoutSpace& space) {
    return measureFixedWidth(renderer, space, kDialogWidth * m_scale);
  }

  void RevertDialogModal::arrangeContent(Renderer& renderer, float width, float height) {
    arrangeRoot(renderer, width, height);
  }

} // namespace settings::display
