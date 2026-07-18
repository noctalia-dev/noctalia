#include "shell/osd/keyboard_backlight_osd.h"

#include "shell/osd/osd_overlay.h"
#include "system/keyboard_backlight_service.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

  const char* kbdBacklightIcon(float brightness) {
    if (brightness <= 0.0f) {
      return "keyboard-off";
    }
    return "keyboard";
  }

  OsdContent makeKbdBacklightContent(float brightness) {
    const float progress = std::clamp(brightness, 0.0f, 1.0f);
    const int percent = static_cast<int>(std::round(progress * 100.0f));
    return OsdContent{
        .kind = OsdKind::KeyboardBacklight,
        .icon = kbdBacklightIcon(progress),
        .value = std::to_string(percent) + "%",
        .progress = progress,
    };
  }

} // namespace

void KeyboardBacklightOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void KeyboardBacklightOsd::onBrightnessChanged(const KeyboardBacklightService& service) {
  if (!service.available()) {
    return;
  }
  const int brightness = service.brightness();
  const int maxBrightness = service.maxBrightness();
  if (m_overlay != nullptr) {
    const float normalized =
        maxBrightness > 0 ? static_cast<float>(brightness) / static_cast<float>(maxBrightness) : 0.0f;
    m_overlay->show(makeKbdBacklightContent(normalized));
  }
}

void KeyboardBacklightOsd::showValue(float brightness) {
  if (m_overlay != nullptr) {
    m_overlay->show(makeKbdBacklightContent(brightness));
  }
}
