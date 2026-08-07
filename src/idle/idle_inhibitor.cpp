#include "idle/idle_inhibitor.h"

#include "core/log.h"
#include "dbus/logind/logind_service.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "ipc/ipc_service.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <string_view>

namespace {

  constexpr Logger kLog("idle");

  [[nodiscard]] bool isInternalDisplayConnector(std::string_view name) {
    return name.starts_with("eDP") || name.starts_with("LVDS") || name.starts_with("DSI");
  }

} // namespace

IdleInhibitor::IdleInhibitor() = default;

IdleInhibitor::~IdleInhibitor() {
  if (m_logind != nullptr) {
    m_logind->setLidStateCallback({});
  }
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

void IdleInhibitor::setLogindService(LogindService* logind) {
  if (m_logind != nullptr) {
    m_logind->setLidStateCallback({});
  }
  m_logind = logind;
  if (m_logind != nullptr) {
    m_logind->setLidStateCallback([this](bool closed) { handleLidState(closed); });
  }
}

void IdleInhibitor::setLidHandlingEnabled(bool enabled) {
  if (m_lidHandlingEnabled == enabled) {
    return;
  }
  m_lidHandlingEnabled = enabled;
  if (m_outputPowerCallback) {
    if (!enabled && m_screenOffForLid) {
      m_screenOffForLid = false;
      (void)m_outputPowerCallback(true);
    } else if (enabled && m_enabled && m_lidClosed && !m_screenOffForLid && hasSingleInternalOutput()) {
      m_screenOffForLid = m_outputPowerCallback(false);
    }
  }
  if (m_enabled) {
    releaseLogindInhibit();
    syncInhibitor(false);
  }
}

void IdleInhibitor::setOutputPowerCallback(OutputPowerCallback callback) {
  m_outputPowerCallback = std::move(callback);
}

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
  if (m_enabled
      && m_lidHandlingEnabled
      && m_lidClosed
      && !m_screenOffForLid
      && m_outputPowerCallback
      && hasSingleInternalOutput()) {
    m_screenOffForLid = m_outputPowerCallback(false);
  }
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

  if (m_logind->acquireIdleInhibit(m_lidHandlingEnabled) && logTransitions && !m_loggedLogindEnable) {
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

void IdleInhibitor::handleLidState(bool closed) {
  if (m_lidStateKnown && m_lidClosed == closed) {
    return;
  }
  m_lidStateKnown = true;
  m_lidClosed = closed;
  if (!m_lidHandlingEnabled) {
    return;
  }
  if (!m_outputPowerCallback) {
    return;
  }
  if (closed) {
    if (!m_enabled) {
      return;
    }
    // Global DPMS commands would blank external displays. In a docked setup the compositor owns
    // lid-driven output topology, including any user-defined switch bindings.
    if (!hasSingleInternalOutput()) {
      return;
    }
    m_screenOffForLid = m_outputPowerCallback(false);
    if (!m_screenOffForLid) {
      kLog.warn("failed to turn screen off after laptop lid closed");
    }
    return;
  }
  if (m_screenOffForLid) {
    m_screenOffForLid = false;
    if (!m_outputPowerCallback(true)) {
      kLog.warn("failed to turn screen on after laptop lid opened");
    }
  }
}

bool IdleInhibitor::hasSingleInternalOutput() const {
  if (m_wayland == nullptr || m_wayland->outputs().size() != 1) {
    return false;
  }
  const WaylandOutput& output = m_wayland->outputs().front();
  return isInternalDisplayConnector(output.connectorName) || isInternalDisplayConnector(output.interfaceName);
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
      "", "Enable caffeine (idle inhibitor)"
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
      "", "Disable caffeine (idle inhibitor)"
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
      "", "Toggle caffeine (idle inhibitor)"
  );
}
