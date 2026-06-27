#include "shell/osd/lock_keys_osd.h"

#include "i18n/i18n.h"
#include "shell/osd/osd_overlay.h"
#include "system/lock_keys_service.h"

namespace {

  enum class LockKeyKind {
    Caps,
    Num,
    Scroll,
  };

  OsdContent makeLockKeyContent(LockKeyKind kind, bool on) {
    switch (kind) {
    case LockKeyKind::Caps:
      return OsdContent{
          .kind = OsdKind::LockKeys,
          .icon = "capslock",
          .value = i18n::tr(on ? "osd.lock-keys.caps-on" : "osd.lock-keys.caps-off"),
          .showProgress = false,
          .inactive = !on,
      };
    case LockKeyKind::Num:
      return OsdContent{
          .kind = OsdKind::LockKeys,
          .icon = "numlock",
          .value = i18n::tr(on ? "osd.lock-keys.num-on" : "osd.lock-keys.num-off"),
          .showProgress = false,
          .inactive = !on,
      };
    case LockKeyKind::Scroll:
      return OsdContent{
          .kind = OsdKind::LockKeys,
          .icon = "scrolllock",
          .value = i18n::tr(on ? "osd.lock-keys.scroll-on" : "osd.lock-keys.scroll-off"),
          .showProgress = false,
          .inactive = !on,
      };
    }

    return OsdContent{};
  }

} // namespace

void LockKeysOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }

void LockKeysOsd::primeFromService(const LockKeysService& service) {
  m_lastState = service.state();
  m_hasState = true;
}

void LockKeysOsd::onLockKeysChanged(
    const WaylandSeat::LockKeysState& previous, const WaylandSeat::LockKeysState& current
) {
  if (!m_hasState) {
    m_lastState = current;
    m_hasState = true;
    return;
  }

  if (m_overlay == nullptr) {
    m_lastState = current;
    return;
  }

  if (previous.capsLock != current.capsLock) {
    m_overlay->show(makeLockKeyContent(LockKeyKind::Caps, current.capsLock));
  } else if (previous.numLock != current.numLock) {
    m_overlay->show(makeLockKeyContent(LockKeyKind::Num, current.numLock));
  } else if (previous.scrollLock != current.scrollLock) {
    m_overlay->show(makeLockKeyContent(LockKeyKind::Scroll, current.scrollLock));
  }

  m_lastState = current;
}
