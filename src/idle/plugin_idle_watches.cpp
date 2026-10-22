#include "idle/plugin_idle_watches.h"

#include "core/log.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr Logger kLog("plugin-idle");

  const ext_idle_notification_v1_listener kListener = {
      .idled = &PluginIdleWatches::handleIdled,
      .resumed = &PluginIdleWatches::handleResumed,
  };

} // namespace

PluginIdleWatches::~PluginIdleWatches() {
  for (auto& watch : m_watches) {
    disarm(*watch);
  }
}

void PluginIdleWatches::initialize(WaylandConnection& wayland) { m_wayland = &wayland; }

void PluginIdleWatches::onCapabilitiesReady() {
  for (auto& watch : m_watches) {
    arm(*watch);
  }
}

void PluginIdleWatches::setScreenSaverInhibited(bool inhibited) {
  if (m_screenSaverInhibited == inhibited) {
    return;
  }
  m_screenSaverInhibited = inhibited;
  for (auto& watch : m_watches) {
    if (watch->ignoreInhibitors) {
      continue;
    }
    if (inhibited && watch->reportedIdle) {
      watch->idledWhileInhibited = true;
      report(*watch, false);
    } else if (!inhibited && watch->idledWhileInhibited) {
      // The compositor is still idle and will not repeat `idled`; restart the timeout from now.
      arm(*watch);
    }
  }
}

void PluginIdleWatches::watch(
    std::uint64_t hostId, int watchId, std::uint32_t timeoutMs, bool ignoreInhibitors, Delivery delivery
) {
  unwatch(hostId, watchId);
  auto watch = std::make_unique<Watch>();
  watch->owner = this;
  watch->hostId = hostId;
  watch->watchId = watchId;
  watch->timeoutMs = timeoutMs;
  watch->ignoreInhibitors = ignoreInhibitors;
  watch->delivery = std::move(delivery);
  arm(*watch);
  m_watches.push_back(std::move(watch));
}

void PluginIdleWatches::unwatch(std::uint64_t hostId, int watchId) {
  std::erase_if(m_watches, [hostId, watchId](auto& watch) {
    if (watch->hostId != hostId || watch->watchId != watchId) {
      return false;
    }
    disarm(*watch);
    return true;
  });
}

void PluginIdleWatches::release(std::uint64_t hostId) {
  std::erase_if(m_watches, [hostId](auto& watch) {
    if (watch->hostId != hostId) {
      return false;
    }
    disarm(*watch);
    return true;
  });
}

void PluginIdleWatches::arm(Watch& watch) {
  disarm(watch);
  watch.idledWhileInhibited = false;
  if (m_wayland == nullptr || !m_wayland->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    return;
  }

  if (watch.ignoreInhibitors) {
    watch.notification = m_wayland->createInputIdleNotification(watch.timeoutMs);
    if (watch.notification == nullptr && !m_warnedInputIdleUnsupported) {
      m_warnedInputIdleUnsupported = true;
      kLog.warn("compositor lacks ext-idle-notify v2; plugin idle watches will honor idle inhibitors");
    }
  }
  if (watch.notification == nullptr) {
    watch.notification = m_wayland->createIdleNotification(watch.timeoutMs);
  }
  if (watch.notification == nullptr) {
    kLog.warn("failed to register plugin idle watch ({}ms)", watch.timeoutMs);
    return;
  }
  ext_idle_notification_v1_add_listener(watch.notification, &kListener, &watch);
}

void PluginIdleWatches::disarm(Watch& watch) {
  if (watch.notification != nullptr) {
    ext_idle_notification_v1_destroy(watch.notification);
    watch.notification = nullptr;
  }
}

void PluginIdleWatches::report(Watch& watch, bool idle) {
  if (watch.reportedIdle == idle) {
    return;
  }
  watch.reportedIdle = idle;
  if (watch.delivery) {
    watch.delivery(idle);
  }
}

void PluginIdleWatches::handleIdled(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* watch = static_cast<Watch*>(data);
  if (!watch->ignoreInhibitors && watch->owner->m_screenSaverInhibited) {
    watch->idledWhileInhibited = true;
    return;
  }
  watch->owner->report(*watch, true);
}

void PluginIdleWatches::handleResumed(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* watch = static_cast<Watch*>(data);
  watch->idledWhileInhibited = false;
  watch->owner->report(*watch, false);
}
