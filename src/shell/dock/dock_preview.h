#pragma once

#include "shell/controls/window_preview_popup.h"
#include "shell/dock/dock_model.h"

class IconResolver;

namespace shell::dock {

  struct DockInstance;

  class DockPreview : public WindowPreviewPopup {
  public:
    DockPreview(
        DockInstance& owner, CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
        IconResolver& icons, std::function<void()> dismiss
    );

    bool show(const DockItemModel& item, const InputArea& anchor);
    DockInstance& owner() const { return m_owner; }
    const std::string& app() const { return m_app; }

  private:
    DockInstance& m_owner;
    CompositorPlatform& m_platform;
    ConfigService& m_config;
    IconResolver& m_icons;
    std::string m_app;
  };

} // namespace shell::dock
