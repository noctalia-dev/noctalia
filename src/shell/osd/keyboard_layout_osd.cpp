#include "shell/osd/keyboard_layout_osd.h"

#include "compositors/compositor_platform.h"
#include "config/config_types.h"
#include "dbus/tray/tray_service.h"
#include "shell/keyboard_layout_label.h"
#include "shell/osd/osd_overlay.h"

#include <string>
#include <utility>

namespace {

  OsdContent makeKeyboardLayoutContent(std::string value) {
    return OsdContent{
        .kind = OsdKind::KeyboardLayout,
        .icon = "keyboard",
        .value = std::move(value),
        .showProgress = false,
    };
  }

  OsdContent makeLayoutContent(const std::string& layoutName, const Config& config) {
    return makeKeyboardLayoutContent(resolveKeyboardLayoutLabel(
        layoutName, KeyboardLayoutDisplayMode::Short, config.shell.keyboardLayout.customLabels
    ));
  }

  OsdContent makeInputMethodContent(const FcitxInputMethodState& state, const Config& config) {
    return makeKeyboardLayoutContent(
        resolveInputMethodLabel(state.label, KeyboardLayoutDisplayMode::Short, config.shell.keyboardLayout.customLabels)
    );
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
  if (m_inputMethodTracker.available()) {
    return;
  }
  if (m_overlay == nullptr) {
    return;
  }

  m_overlay->show(makeLayoutContent(layoutName, config));
}

void KeyboardLayoutOsd::onTrayChanged(const TrayService& tray, const Config& config, bool enabled) {
  const auto changed = m_inputMethodTracker.update(tray.items());
  if (!changed.has_value()) {
    return;
  }

  if (!enabled || m_overlay == nullptr) {
    return;
  }

  m_overlay->show(makeInputMethodContent(*changed, config));
}
