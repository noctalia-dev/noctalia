#pragma once

#include "shell/settings/display/modal_base.h"

class Renderer;

namespace settings::display {

  class DisplayService;

  // "Keep these display settings?" countdown stacked above the monitor editor.
  class RevertDialogModal final : public settings::ModalBase {
  public:
    void open(SettingsModalHost& host, DisplayService& display, float scale);

  private:
    [[nodiscard]] std::unique_ptr<Node> buildContent() override;
    [[nodiscard]] LayoutSize measureContent(Renderer& renderer, const SettingsModalLayoutSpace& space) override;
    void arrangeContent(Renderer& renderer, float width, float height) override;
    void updateContent(Renderer& renderer) override;
    void onClosed() override;

    DisplayService* m_display = nullptr;
    float m_scale = 1.0F;
    int m_lastCountdown = -1;
  };

} // namespace settings::display
