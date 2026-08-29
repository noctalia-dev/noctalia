#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <sdbus-c++/TypeTraits.h>
#include <string>
#include <string_view>
#include <vector>

class SystemBus;

namespace sdbus {
  class IProxy;
}

// systemd-networkd's view of the machine's links.
//
// A supplicant backend (iwd, wpa_supplicant) speaks only 802.11: it knows
// nothing about a wired link and nothing about the addresses on any link.
// On a host running one of those, networkd owns both, so it is the only place
// to ask. Composed into such a backend rather than being one itself — it has
// no notion of a connection to activate or a network to join.
class NetworkdLinkMonitor {
public:
  struct Link {
    std::string name;
    std::string objectPath;
    std::string operationalState;
    std::string ipv4; // dotted-quad of the first IPv4 address; empty if none
    bool wireless = false;

    [[nodiscard]] bool connected() const noexcept;
  };

  using ChangeCallback = std::function<void()>;

  // Throws when systemd-networkd is not on the bus.
  explicit NetworkdLinkMonitor(SystemBus& bus);
  ~NetworkdLinkMonitor();

  NetworkdLinkMonitor(const NetworkdLinkMonitor&) = delete;
  NetworkdLinkMonitor& operator=(const NetworkdLinkMonitor&) = delete;

  void setChangeCallback(ChangeCallback callback);
  void refresh();

  [[nodiscard]] const std::vector<Link>& links() const noexcept { return m_links; }
  [[nodiscard]] const Link* primaryWired() const noexcept;
  [[nodiscard]] const Link* findLink(std::string_view name) const noexcept;

  // Reapplies the link's .network configuration, the closest systemd-networkd
  // offers to "bring this connection up". Dispatched asynchronously; the real
  // result arrives via the change callback.
  bool reconfigure(const Link& link);

private:
  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_manager;
  std::unique_ptr<sdbus::IProxy> m_reconfigureProxy; // keeps an in-flight async Reconfigure alive
  sdbus::Slot m_linkPropertiesSlot;
  std::vector<Link> m_links;
  ChangeCallback m_changeCallback;
};

namespace networkd_links {

  // networkd reports "routable" for a link with an address and a route, and
  // "carrier" for one that is up but configured elsewhere. Every other state
  // ("degraded", "no-carrier", "off", ...) cannot carry traffic.
  //
  // This answers only whether a link can carry traffic, never whether it is one
  // worth offering: loopback is permanently "carrier". Classify first and test
  // the state second, or lo becomes the primary wired link on a host with the
  // cable out.
  [[nodiscard]] bool isConnectedState(std::string_view operationalState) noexcept;

  struct Classification {
    bool physical = false; // a real adapter rather than loopback, a tunnel, or a software device
    bool wireless = false; // the kernel gave it an 802.11 phy
  };

  // Classifies by what the kernel says the interface is, never by its name.
  // A name-prefix test misreads any host that renames its NICs, and renaming
  // is the normal way to get stable names out of systemd.link or udev.
  [[nodiscard]] Classification
  classify(std::string_view ifname, const std::filesystem::path& sysClassNet = "/sys/class/net");

  // Links arrive from networkd in no meaningful order, so prefer a connected
  // one and fall back to the first of that kind seen.
  [[nodiscard]] const NetworkdLinkMonitor::Link*
  selectPrimary(const std::vector<NetworkdLinkMonitor::Link>& links, bool wireless) noexcept;

} // namespace networkd_links
