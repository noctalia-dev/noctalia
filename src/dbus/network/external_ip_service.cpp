#include "dbus/network/external_ip_service.h"

#include "config/config_service.h"
#include "dbus/network/inetwork_service.h"
#include "net/http_client.h"
#include "util/string_utils.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace {

  // Identity of the route the WAN IP was resolved through.
  std::string wanProbeKey(const NetworkState& s) { return s.ipv4 + (s.vpnConnected ? "|vpn" : "|"); }

  // The fetch endpoint returns the address as plain text; accept only strings
  // that look like an IPv4/IPv6 literal before displaying them.
  bool isPlausibleIpLiteral(std::string_view s) {
    if (s.empty() || s.size() > 45) {
      return false;
    }
    return std::ranges::all_of(s, [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
    });
  }

} // namespace

ExternalIpService::ExternalIpService(HttpClient* http, ConfigService* config) : m_http(http), m_config(config) {}

bool ExternalIpService::enabled() const {
  if (m_config == nullptr) {
    return false;
  }
  const auto& shell = m_config->config().shell;
  return shell.externalIpEnabled && !shell.offlineMode;
}

void ExternalIpService::onNetworkChanged() { sync(); }

void ExternalIpService::onConfigReload() { sync(); }

void ExternalIpService::setValue(std::string ip) {
  if (ip == m_externalIp) {
    return;
  }
  m_externalIp = std::move(ip);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ExternalIpService::sync() {
  if (m_network == nullptr || !enabled()) {
    m_timer.stop();
    setValue({});
    return;
  }
  const NetworkState& s = m_network->state();
  if (s.connected) {
    // A network or VPN change invalidates the resolved WAN IP right away.
    if (wanProbeKey(s) != m_externalIpKey) {
      setValue({});
    }
    maybeScheduleProbe();
  } else {
    setValue({});
    // Drop the backoff so a reconnect probes promptly.
    m_fetchedAt = std::chrono::steady_clock::time_point{};
  }
}

void ExternalIpService::maybeScheduleProbe() {
  if (m_http == nullptr || m_network == nullptr || !enabled()) {
    return;
  }
  const NetworkState& s = m_network->state();
  if (!s.connected || m_fetchInFlight || m_timer.active()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (wanProbeKey(s) == m_externalIpKey) {
    if (!m_externalIp.empty() && now - m_fetchedAt < std::chrono::minutes(5)) {
      return;
    }
    // Failure backoff: don't hammer the endpoint from every change.
    if (now - m_fetchedAt < std::chrono::seconds(30)) {
      return;
    }
  }
  // Wait out the route/DNS settle window after a connection or VPN change —
  // probing the instant NM reports the change can still egress the old path.
  m_timer.start(std::chrono::milliseconds(2000), [this] { probeNow(); });
}

void ExternalIpService::probeNow() {
  if (m_http == nullptr || m_network == nullptr || !enabled() || m_fetchInFlight) {
    return;
  }
  const NetworkState& s = m_network->state();
  if (!s.connected) {
    return;
  }

  m_fetchInFlight = true;
  const std::string key = wanProbeKey(s);
  const std::weak_ptr<int> alive = m_lifetime;
  HttpRequest req;
  req.url = "https://api.noctalia.dev/ip";
  // A pooled keep-alive connection predating a VPN/route change would answer
  // via the old path and report the stale address.
  req.freshConnection = true;
  m_http->request(std::move(req), [this, alive, key](HttpResponse response) {
    if (alive.expired()) {
      return;
    }
    m_fetchInFlight = false;
    m_fetchedAt = std::chrono::steady_clock::now();
    m_externalIpKey = key;
    if (!enabled()) {
      // The setting (or offline mode) flipped while the probe was in flight.
      setValue({});
      return;
    }
    std::string ip;
    if (response.transportOk && response.status == 200) {
      const std::string trimmed = StringUtils::trim(response.body);
      if (isPlausibleIpLiteral(trimmed)) {
        ip = trimmed;
      }
    }
    setValue(ip);
    if (!m_externalIp.empty() && m_externalIpConfirmedKey != key) {
      // One confirmation probe per network/VPN state: the first probe after a
      // transition can race a still-settling route and return the old address.
      m_externalIpConfirmedKey = key;
      m_timer.start(std::chrono::milliseconds(4000), [this] { probeNow(); });
    }
  });
}
