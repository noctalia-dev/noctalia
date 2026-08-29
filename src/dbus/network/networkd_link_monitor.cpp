#include "dbus/network/networkd_link_monitor.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <optional>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace {

  constexpr Logger kLog("networkd");

  const sdbus::ServiceName kNetworkdBusName{"org.freedesktop.network1"};
  const sdbus::ObjectPath kManagerPath{"/org/freedesktop/network1"};
  constexpr auto kManagerInterface = "org.freedesktop.network1.Manager";
  constexpr auto kLinkInterface = "org.freedesktop.network1.Link";

  // One rule covers every link object, including links that appear later.
  // arg0 is PropertiesChanged's interface name, so filtering on it here keeps
  // manager-level property churn from waking a refresh.
  constexpr auto kLinkPropertiesMatch = "type='signal',"
                                        "sender='org.freedesktop.network1',"
                                        "interface='org.freedesktop.DBus.Properties',"
                                        "member='PropertiesChanged',"
                                        "path_namespace='/org/freedesktop/network1',"
                                        "arg0='org.freedesktop.network1.Link'";

  constexpr int kArphrdEther = 1;

  using LinkTriple = sdbus::Struct<std::int32_t, std::string, sdbus::ObjectPath>;

  std::optional<int> readIntFile(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
      return std::nullopt;
    }
    std::string text;
    if (!std::getline(in, text)) {
      return std::nullopt;
    }
    int value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    if (std::from_chars(first, last, value).ec != std::errc{}) {
      return std::nullopt;
    }
    return value;
  }

  std::unordered_map<std::string, std::string> firstIpv4PerInterface() {
    std::unordered_map<std::string, std::string> addresses;
    ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) {
      kLog.debug("getifaddrs failed: {}", std::strerror(errno));
      return addresses;
    }
    for (const ifaddrs* ifa = head; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr || ifa->ifa_name == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
        continue;
      }
      std::array<char, INET_ADDRSTRLEN> text{};
      const auto* address = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
      if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()) == nullptr) {
        continue;
      }
      // First address wins; an interface with several keeps the one the kernel
      // lists first, which is the one the UI has room to show.
      addresses.emplace(ifa->ifa_name, text.data());
    }
    freeifaddrs(head);
    return addresses;
  }

} // namespace

bool NetworkdLinkMonitor::Link::connected() const noexcept {
  return networkd_links::isConnectedState(operationalState);
}

NetworkdLinkMonitor::NetworkdLinkMonitor(SystemBus& bus) : m_bus(bus) {
  if (!bus.nameHasOwner("org.freedesktop.network1")) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.ServiceUnknown"},
        "The name org.freedesktop.network1 was not provided by any .service files"
    );
  }

  m_manager = sdbus::createProxy(m_bus.connection(), kNetworkdBusName, kManagerPath);
  m_linkPropertiesSlot = m_bus.connection().addMatch(
      kLinkPropertiesMatch,
      [this](sdbus::Message) {
        refresh();
        if (m_changeCallback) {
          m_changeCallback();
        }
      },
      sdbus::return_slot
  );

  refresh();
}

NetworkdLinkMonitor::~NetworkdLinkMonitor() = default;

void NetworkdLinkMonitor::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void NetworkdLinkMonitor::refresh() {
  std::vector<LinkTriple> triples;
  try {
    m_manager->callMethod("ListLinks").onInterface(kManagerInterface).storeResultsTo(triples);
  } catch (const sdbus::Error& e) {
    kLog.warn("ListLinks failed: {}", e.what());
    return;
  }

  const auto addresses = firstIpv4PerInterface();

  std::vector<Link> links;
  links.reserve(triples.size());
  for (const auto& [ifindex, name, path] : triples) {
    (void)ifindex;
    const auto classification = networkd_links::classify(name);
    if (!classification.physical) {
      continue;
    }

    Link link;
    link.name = name;
    link.objectPath = std::string(path);
    link.wireless = classification.wireless;
    if (const auto address = addresses.find(name); address != addresses.end()) {
      link.ipv4 = address->second;
    }
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kNetworkdBusName, path);
      const sdbus::Variant value = proxy->getProperty("OperationalState").onInterface(kLinkInterface);
      link.operationalState = value.get<std::string>();
    } catch (const sdbus::Error& e) {
      kLog.debug("OperationalState unavailable on {}: {}", link.name, e.what());
    }
    links.push_back(std::move(link));
  }

  m_links = std::move(links);
}

const NetworkdLinkMonitor::Link* NetworkdLinkMonitor::primaryWired() const noexcept {
  return networkd_links::selectPrimary(m_links, false);
}

const NetworkdLinkMonitor::Link* NetworkdLinkMonitor::findLink(std::string_view name) const noexcept {
  const auto it = std::ranges::find(m_links, name, &Link::name);
  return it == m_links.end() ? nullptr : &*it;
}

bool NetworkdLinkMonitor::reconfigure(const Link& link) {
  try {
    // Reconfigure is polkit-gated, and a synchronous call would freeze the main
    // loop while the agent prompts. Starting a new one supersedes any previous.
    m_reconfigureProxy = sdbus::createProxy(m_bus.connection(), kNetworkdBusName, sdbus::ObjectPath{link.objectPath});
    m_reconfigureProxy->callMethodAsync("Reconfigure")
        .onInterface(kLinkInterface)
        .uponReplyInvoke([this, name = link.name](std::optional<sdbus::Error> error) {
          if (error) {
            kLog.warn("Reconfigure failed on {}: {}", name, error->what());
            return;
          }
          refresh();
          if (m_changeCallback) {
            m_changeCallback();
          }
        });
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("Reconfigure dispatch failed on {}: {}", link.name, e.what());
    return false;
  }
}

namespace networkd_links {

  bool isConnectedState(std::string_view operationalState) noexcept {
    return operationalState == "routable" || operationalState == "carrier";
  }

  Classification classify(std::string_view ifname, const std::filesystem::path& sysClassNet) {
    const std::filesystem::path base = sysClassNet / ifname;
    std::error_code ec;

    Classification classification;
    // Two tests together, because neither is enough alone. The hardware type
    // rules out loopback (ARPHRD_LOOPBACK) and the tunnels a VPN leaves behind
    // (ARPHRD_NONE), but bridges, veth and tap devices all report ARPHRD_ETHER
    // like a real NIC. What they lack is the driver-model link every piece of
    // hardware has, so require that too — and no list of virtual name prefixes
    // is needed to keep in step with whatever the next container runtime names
    // its bridge.
    classification.physical =
        readIntFile(base / "type") == kArphrdEther && std::filesystem::exists(base / "device", ec);
    // phy80211 is what cfg80211 drivers expose; wireless is the older
    // wireless-extensions node some drivers still offer on its own.
    classification.wireless =
        std::filesystem::exists(base / "phy80211", ec) || std::filesystem::exists(base / "wireless", ec);
    return classification;
  }

  const NetworkdLinkMonitor::Link*
  selectPrimary(const std::vector<NetworkdLinkMonitor::Link>& links, bool wireless) noexcept {
    const NetworkdLinkMonitor::Link* fallback = nullptr;
    for (const auto& link : links) {
      if (link.wireless != wireless) {
        continue;
      }
      if (link.connected()) {
        return &link;
      }
      if (fallback == nullptr) {
        fallback = &link;
      }
    }
    return fallback;
  }

} // namespace networkd_links
