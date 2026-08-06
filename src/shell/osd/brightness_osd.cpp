#include "shell/osd/brightness_osd.h"

#include "shell/osd/osd_overlay.h"
#include "system/brightness_service.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

  const char* brightnessIconName(float brightness) {
    if (brightness < 0.4F) {
      return "brightness-low";
    }
    return "brightness-high";
  }

  OsdContent makeBrightnessContent(float brightness) {
    const int percent = static_cast<int>(std::round(std::max(0.0F, brightness) * 100.0F));
    return OsdContent{
        .kind = OsdKind::Brightness,
        .icon = brightnessIconName(brightness),
        .value = std::to_string(percent) + "%",
        .progress = std::clamp(brightness, 0.0F, 1.0F),
    };
  }

} // namespace

void BrightnessOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void BrightnessOsd::primeFromService(const BrightnessService& service) {
  m_snapshots.clear();
  for (const auto& display : service.displays()) {
    if (!display.controllable) {
      continue;
    }
    m_snapshots.push_back({
        .id = display.id,
        .percent = static_cast<int>(std::round(std::max(0.0F, display.brightness) * 100.0F)),
    });
  }
}

void BrightnessOsd::suppressFor(std::chrono::milliseconds duration) {
  m_suppressUntil = std::chrono::steady_clock::now() + duration;
}

void BrightnessOsd::onBrightnessChanged(const BrightnessService& service) {
  const auto& displays = service.displays();
  const auto now = std::chrono::steady_clock::now();

  // Find the display whose brightness actually changed
  const BrightnessDisplay* changed = nullptr;
  for (const auto& display : displays) {
    if (!display.controllable) {
      continue;
    }
    const int percent = static_cast<int>(std::round(std::max(0.0F, display.brightness) * 100.0F));

    // Find previous snapshot
    bool found = false;
    for (auto& snap : m_snapshots) {
      if (snap.id == display.id) {
        if (snap.percent != percent) {
          changed = &display;
          snap.percent = percent;
        }
        found = true;
        break;
      }
    }

    // New display appeared
    if (!found) {
      m_snapshots.push_back({.id = display.id, .percent = percent});
    }
  }

  if (changed == nullptr || now < m_suppressUntil) {
    return;
  }

  if (m_overlay != nullptr) {
    m_overlay->show(makeBrightnessContent(changed->brightness));
  }
}

void BrightnessOsd::showValue(float brightness) {
  if (m_overlay == nullptr) {
    return;
  }
  m_overlay->show(makeBrightnessContent(brightness));
}
