#include "system/hardware_info.h"

#include "compositors/compositor_detect.h"
#include "i18n/i18n.h"

#include <cstdlib>
#include <string>

std::string compositorLabel() {
  const auto kind = compositors::detect();
  if (kind != compositors::CompositorKind::Unknown) {
    return std::string(compositors::name(kind));
  }
  if (const char* desktop = std::getenv("XDG_CURRENT_DESKTOP"); desktop != nullptr && desktop[0] != '\0') {
    return desktop;
  }
  if (const char* sessionDesktop = std::getenv("XDG_SESSION_DESKTOP");
      sessionDesktop != nullptr && sessionDesktop[0] != '\0') {
    return sessionDesktop;
  }
  return i18n::tr("system.hardware.unknown");
}
