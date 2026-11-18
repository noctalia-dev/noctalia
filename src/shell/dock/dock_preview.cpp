#include "shell/dock/dock_preview.h"

#include "config/config_service.h"
#include "shell/dock/dock_geometry.h"
#include "shell/dock/dock_instance.h"
#include "shell/wayland/layer_surface.h"
#include "system/icon_resolver.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace shell::dock {

  DockPreview::DockPreview(
      DockInstance& owner, CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext,
      IconResolver& icons, std::function<void()> dismiss
  )
      : WindowPreviewPopup(platform, config, renderContext, std::move(dismiss)), m_owner(owner), m_platform(platform),
        m_config(config), m_icons(icons) {}

  bool DockPreview::show(const DockItemModel& item, const InputArea& anchor) {
    const auto& cfg = m_owner.config;
    auto& surface = *m_owner.surface;
    const auto& panel = m_owner.panelGeometry;
    const float scale = m_config.config().accessibility.uiScale;
    const float zoomClearance = std::min(Style::spaceSm * scale, static_cast<float>(dockHoverZoomCrossPad(cfg)));
    const bool fromStart = cfg.position == DockEdge::Top || cfg.position == DockEdge::Left;
    const bool vertical = isVerticalEdge(cfg.position);
    const auto thickness = static_cast<float>(vertical ? surface.width() : surface.height());
    const float panelStart = vertical ? panel.panelX : panel.panelY;
    const float panelSize = vertical ? panel.panelW : panel.panelH;
    const float face = fromStart ? panelStart + panelSize : panelStart;
    const float depth = fromStart ? face : thickness - face;

    m_app = item.idLower;
    const auto iconSize = static_cast<int>(std::ceil(96.0F * scale));
    std::string icon = m_icons.resolve(item.entry.icon, iconSize);
    if (icon.empty()) {
      icon = m_icons.resolve("application-x-executable", iconSize);
    }
    return WindowPreviewPopup::show(
        {surface, m_owner.output, cfg.position, face + (fromStart ? zoomClearance : -zoomClearance),
         depth + static_cast<float>(cfg.marginEdge), cfg.windowPreviewWidth},
        anchor, item.entry.name, std::move(icon),
        [this, id = item.windowLookupIdLower, wmClass = item.windowLookupWmClassLower] {
          return windowsForDockItem(m_platform, id, wmClass, nullptr);
        }
    );
  }

} // namespace shell::dock
