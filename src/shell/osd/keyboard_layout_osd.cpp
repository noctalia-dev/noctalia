#include "shell/osd/keyboard_layout_osd.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "shell/bar/widgets/keyboard_layout_widget.h"
#include "shell/osd/osd_overlay.h"

namespace {

  OsdContent makeKeyboardLayoutContent(const std::string& layoutName, const Config& config) {
    std::string label;
    const auto widgetIt = config.widgets.find("keyboard_layout");
    if (widgetIt != config.widgets.end()) {
      const auto& wc = widgetIt->second;
      const auto labelsIt = wc.tables.find("custom_labels");
      if (labelsIt != wc.tables.end()) {
        const auto entryIt = labelsIt->second.find(layoutName);
        if (entryIt != labelsIt->second.end() && !entryIt->second.empty()) {
          label = entryIt->second;
        }
      }
      if (label.empty()) {
        const auto modeIt = wc.settings.find("display");
        KeyboardLayoutWidget::DisplayMode mode = KeyboardLayoutWidget::DisplayMode::Short;
        if (modeIt != wc.settings.end()) {
          if (const auto* s = std::get_if<std::string>(&modeIt->second)) {
            mode = KeyboardLayoutWidget::parseDisplayMode(*s);
          }
        }
        if (mode != KeyboardLayoutWidget::DisplayMode::Custom) {
          label = KeyboardLayoutWidget::formatLayoutLabel(layoutName, mode);
        } else {
          label = layoutName;
        }
      }
    } else {
      label = KeyboardLayoutWidget::formatLayoutLabel(layoutName, KeyboardLayoutWidget::DisplayMode::Short);
    }
    return OsdContent{
        .icon = "keyboard",
        .value = label,
        .showProgress = false,
    };
  }

} // namespace

void KeyboardLayoutOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void KeyboardLayoutOsd::prime(const CompositorPlatform& platform) {
  m_lastLayoutName = platform.currentKeyboardLayoutName();
  m_hasLayout = true;
}

void KeyboardLayoutOsd::onLayoutChanged(const CompositorPlatform& platform, const Config& config) {
  const std::string layoutName = platform.currentKeyboardLayoutName();
  if (layoutName.empty()) {
    return;
  }

  if (!m_hasLayout) {
    m_lastLayoutName = layoutName;
    m_hasLayout = true;
    return;
  }

  if (layoutName == m_lastLayoutName) {
    return;
  }

  m_lastLayoutName = layoutName;
  if (m_overlay == nullptr) {
    return;
  }

  m_overlay->show(makeKeyboardLayoutContent(layoutName, config));
}
