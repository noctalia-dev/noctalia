#include "dbus/network/networkd_link_monitor.h"
#include "test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  namespace fs = std::filesystem;

  using Link = NetworkdLinkMonitor::Link;

  // Writes the sysfs nodes the kernel exposes for one interface: the ARP
  // hardware type every net device has, the driver-model link only hardware
  // gets, and the 802.11 phy only a wireless adapter gets.
  void writeInterface(const fs::path& sysClassNet, std::string_view name, int type, bool hasDevice, bool wireless) {
    const fs::path base = sysClassNet / name;
    fs::create_directories(base);
    std::ofstream typeFile{base / "type"};
    typeFile << type << "\n";
    if (hasDevice) {
      fs::create_directories(base / "device");
    }
    if (wireless) {
      fs::create_directories(base / "phy80211");
    }
  }

  Link makeLink(std::string name, std::string operationalState, bool wireless) {
    Link link;
    link.name = std::move(name);
    link.operationalState = std::move(operationalState);
    link.wireless = wireless;
    return link;
  }

  void checkClassification(const fs::path& sysClassNet) {
    // A renamed wired NIC and a renamed wireless one: neither name carries a
    // hint of what it is, which is exactly the case a prefix test gets wrong.
    writeInterface(sysClassNet, "net0", 1, true, false);
    writeInterface(sysClassNet, "net1", 1, true, true);
    writeInterface(sysClassNet, "lo", 772, false, false);
    writeInterface(sysClassNet, "vpn0", 65534, false, false);
    // A container bridge and one end of a veth pair. Both claim to be Ethernet.
    writeInterface(sysClassNet, "docker0", 1, false, false);
    writeInterface(sysClassNet, "veth9f21c3a", 1, false, false);

    const auto wired = networkd_links::classify("net0", sysClassNet);
    TEST_CHECK(wired.physical);
    TEST_CHECK(!wired.wireless);

    const auto wireless = networkd_links::classify("net1", sysClassNet);
    TEST_CHECK(wireless.physical);
    TEST_CHECK(wireless.wireless);

    // Loopback and a VPN tunnel fail on hardware type.
    TEST_CHECK(!networkd_links::classify("lo", sysClassNet).physical);
    TEST_CHECK(!networkd_links::classify("vpn0", sysClassNet).physical);

    // A bridge and a veth pass hardware type and fail on having no hardware,
    // which is what keeps a container runtime's interfaces out of the bar
    // without naming any of them.
    TEST_CHECK(!networkd_links::classify("docker0", sysClassNet).physical);
    TEST_CHECK(!networkd_links::classify("veth9f21c3a", sysClassNet).physical);

    // An interface that has gone away between ListLinks and this read.
    TEST_CHECK(!networkd_links::classify("absent", sysClassNet).physical);
  }

  void checkConnectedStates() {
    TEST_CHECK(networkd_links::isConnectedState("routable"));
    TEST_CHECK(networkd_links::isConnectedState("carrier"));
    TEST_CHECK(!networkd_links::isConnectedState("degraded"));
    TEST_CHECK(!networkd_links::isConnectedState("no-carrier"));
    TEST_CHECK(!networkd_links::isConnectedState("off"));
    TEST_CHECK(!networkd_links::isConnectedState(""));
  }

  void checkPrimarySelection() {
    TEST_CHECK(networkd_links::selectPrimary({}, false) == nullptr);

    // Wi-Fi links are never candidates for the wired slot, however many there are.
    const std::vector<Link> wifiOnly{makeLink("net1", "routable", true)};
    TEST_CHECK(networkd_links::selectPrimary(wifiOnly, false) == nullptr);
    TEST_CHECK(networkd_links::selectPrimary(wifiOnly, true) != nullptr);

    // A connected link wins over one listed earlier, since networkd's order
    // says nothing about which link is carrying traffic.
    const std::vector<Link> twoWired{
        makeLink("net0", "no-carrier", false),
        makeLink("builtin", "routable", false),
    };
    const Link* chosen = networkd_links::selectPrimary(twoWired, false);
    TEST_CHECK(chosen != nullptr);
    TEST_CHECK(chosen->name == "builtin");

    // With nothing connected the first wired link still stands in, so callers
    // have something to offer to reconfigure.
    const std::vector<Link> allDown{
        makeLink("net0", "no-carrier", false),
        makeLink("builtin", "off", false),
    };
    const Link* fallback = networkd_links::selectPrimary(allDown, false);
    TEST_CHECK(fallback != nullptr);
    TEST_CHECK(fallback->name == "net0");
  }

} // namespace

int main() {
  char dirTemplate[] = "/tmp/noctalia_networkd_link_monitor_test_XXXXXX";
  const char* root = mkdtemp(dirTemplate);
  TEST_CHECK(root != nullptr);
  const fs::path sysClassNet{root};

  checkClassification(sysClassNet);
  checkConnectedStates();
  checkPrimarySelection();

  std::error_code ec;
  fs::remove_all(sysClassNet, ec);
  return EXIT_SUCCESS;
}
