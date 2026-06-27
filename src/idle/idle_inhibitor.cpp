#include "idle/idle_inhibitor.h"

#include "core/log.h"
#include "dbus/logind/logind_service.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "ipc/ipc_service.h"
#include "wayland/wayland_connection.h"

#include <algorithm>

namespace {

  constexpr Logger kLog("idle");

} // namespace

IdleInhibitor::IdleInhibitor() = default;

IdleInhibitor::~IdleInhibitor() {
  destroyWaylandInhibitors(false);
  releaseLogindInhibit();
}

bool IdleInhibitor::initialize(WaylandConnection& wayland) {
  m_wayland = &wayland;
  m_manager = m_wayland->idleInhibitManager();

  if (m_manager == nullptr) {
    kLog.info("idle inhibit protocol unavailable");
  }

  return true;
}

void IdleInhibitor::setLogindService(LogindService* logind) { m_logind = logind; }

void IdleInhibitor::setAnchorSurfacesProvider(AnchorSurfacesProvider provider) {
  m_anchorSurfacesProvider = std::move(provider);
}

bool IdleInhibitor::available() const noexcept {
  return m_manager != nullptr || (m_logind != nullptr && m_logind->supportsIdleInhibit());
}

void IdleInhibitor::toggle() { setEnabled(!m_enabled); }

void IdleInhibitor::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }

  const bool wasEnabled = m_enabled;
  m_enabled = enabled;
  if (!m_enabled) {
    m_loggedWaylandEnable = false;
    m_loggedLogindEnable = false;
  }

  syncInhibitor(m_enabled);
  if (wasEnabled && !m_enabled) {
    kLog.info("idle inhibitor disabled");
  }
  notifyChanged();
}

void IdleInhibitor::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void IdleInhibitor::syncInhibitor(bool logTransitions) {
  if (!m_enabled) {
    destroyWaylandInhibitors(false);
    releaseLogindInhibit();
    return;
  }

  syncLogindInhibit(logTransitions);
  if (m_logind != nullptr && m_logind->hasIdleInhibit()) {
    destroyWaylandInhibitors(false);
    return;
  }

  syncWaylandInhibitors(logTransitions);
}

void IdleInhibitor::syncWaylandInhibitors(bool logTransitions) {
  if (m_manager == nullptr) {
    destroyWaylandInhibitors(false);
    return;
  }
  if (!m_anchorSurfacesProvider) {
    destroyWaylandInhibitors(false);
    return;
  }

  const std::vector<wl_surface*> surfaces = m_anchorSurfacesProvider();
  for (auto it = m_inhibitors.begin(); it != m_inhibitors.end();) {
    if (!std::ranges::contains(surfaces, it->first)) {
      zwp_idle_inhibitor_v1_destroy(it->second);
      it = m_inhibitors.erase(it);
    } else {
      ++it;
    }
  }

  for (wl_surface* surface : surfaces) {
    if (surface == nullptr || m_inhibitors.contains(surface)) {
      continue;
    }
    zwp_idle_inhibitor_v1* inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(m_manager, surface);
    if (inhibitor == nullptr) {
      continue;
    }
    m_inhibitors.emplace(surface, inhibitor);
  }

  if (!m_inhibitors.empty() && logTransitions && !m_loggedWaylandEnable) {
    kLog.info("idle inhibitor enabled via wayland ({} surface(s))", m_inhibitors.size());
    m_loggedWaylandEnable = true;
  }
}

void IdleInhibitor::syncLogindInhibit(bool logTransitions) {
  if (m_logind == nullptr || !m_logind->supportsIdleInhibit()) {
    releaseLogindInhibit();
    return;
  }

  if (m_logind->acquireIdleInhibit() && logTransitions && !m_loggedLogindEnable) {
    kLog.info("idle inhibitor enabled via logind");
    m_loggedLogindEnable = true;
  }
}

void IdleInhibitor::destroyWaylandInhibitors(bool /*logDisable*/) {
  if (m_inhibitors.empty()) {
    return;
  }

  for (auto& [_, inhibitor] : m_inhibitors) {
    (void)_;
    zwp_idle_inhibitor_v1_destroy(inhibitor);
  }
  m_inhibitors.clear();
  m_loggedWaylandEnable = false;
}

void IdleInhibitor::releaseLogindInhibit() {
  if (m_logind == nullptr) {
    return;
  }
  m_logind->releaseIdleInhibit();
  m_loggedLogindEnable = false;
}

void IdleInhibitor::notifyChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void IdleInhibitor::onOutputChange() {
  destroyWaylandInhibitors(false);
  releaseLogindInhibit();
  if (m_enabled) {
    syncInhibitor(false);
  }
}

void IdleInhibitor::resyncAnchorSurfaces() {
  if (!m_enabled) {
    return;
  }
  syncInhibitor(false);
}

void IdleInhibitor::registerIpc(IpcService& ipc, StateFeedbackCallback stateFeedback) {
  ipc.registerHandler(
      "caffeine-enable",
      [this, stateFeedback](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        if (m_enabled) {
          return "ok\n";
        }
        setEnabled(true);
        if (stateFeedback) {
          stateFeedback(true);
        }
        return "ok\n";
      },
      "caffeine-enable", "Enable caffeine (idle inhibitor)"
  );

  ipc.registerHandler(
      "caffeine-disable",
      [this, stateFeedback](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        if (!m_enabled) {
          return "ok\n";
        }
        setEnabled(false);
        if (stateFeedback) {
          stateFeedback(false);
        }
        return "ok\n";
      },
      "caffeine-disable", "Disable caffeine (idle inhibitor)"
  );

  ipc.registerHandler(
      "caffeine-toggle",
      [this, stateFeedback](const std::string&) -> std::string {
        if (!available())
          return "error: caffeine protocol unavailable\n";
        const bool nextState = !m_enabled;
        setEnabled(nextState);
        if (stateFeedback) {
          stateFeedback(nextState);
        }
        return "ok\n";
      },
      "caffeine-toggle", "Toggle caffeine (idle inhibitor)"
  );
}
