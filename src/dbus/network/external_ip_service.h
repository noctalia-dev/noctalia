#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

class ConfigService;
class HttpClient;
class INetworkService;

// Resolves the connection's external (WAN) IP and exposes it to any consumer
// (the control-center network tab, the bar network widget). Probing is gated on
// the [shell].externalIpEnabled setting and offline mode; it runs on network /
// VPN changes while connected, then confirms once and refreshes at most every
// five minutes. Keyed on the local IPv4 + VPN tunnel state — a key change clears
// the value and re-probes after a settle delay (the first probe after a
// transition can race the route change and report the old address).
class ExternalIpService {
public:
  ExternalIpService(HttpClient* http, ConfigService* config);

  void setNetworkService(INetworkService* network) { m_network = network; }
  // Invoked when the resolved value changes (cleared or newly resolved).
  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

  [[nodiscard]] const std::string& externalIp() const noexcept { return m_externalIp; }

  // Re-evaluate against the current network state; call on network/VPN changes.
  void onNetworkChanged();
  // Re-evaluate after a config reload (the setting or offline mode may have flipped).
  void onConfigReload();

private:
  [[nodiscard]] bool enabled() const;
  void sync();
  void maybeScheduleProbe();
  void probeNow();
  void setValue(std::string ip);

  HttpClient* m_http = nullptr;
  ConfigService* m_config = nullptr;
  INetworkService* m_network = nullptr;
  std::function<void()> m_changeCallback;

  std::string m_externalIp;
  std::string m_externalIpKey;          // key of the last completed probe
  std::string m_externalIpConfirmedKey; // key that already got its confirmation probe
  std::chrono::steady_clock::time_point m_fetchedAt;
  bool m_fetchInFlight = false;
  Timer m_timer;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
