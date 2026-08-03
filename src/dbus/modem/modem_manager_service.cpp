#include "dbus/modem/modem_manager_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("modem");

  const sdbus::ServiceName kMmBusName{"org.freedesktop.ModemManager1"};
  const sdbus::ObjectPath kRootPath{"/org/freedesktop/ModemManager1"};
  const sdbus::ServiceName kDaemonBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDaemonPath{"/org/freedesktop/DBus"};
  constexpr auto kDaemonInterface = "org.freedesktop.DBus";
  constexpr auto kModemInterface = "org.freedesktop.ModemManager1.Modem";
  constexpr auto kModem3gppInterface = "org.freedesktop.ModemManager1.Modem.Modem3gpp";
  constexpr auto kModemSignalInterface = "org.freedesktop.ModemManager1.Modem.Signal";
  constexpr std::uint32_t kSignalRefreshRateSeconds = 30;
  constexpr auto kObjectManagerInterface = "org.freedesktop.DBus.ObjectManager";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  using InterfaceProps = std::map<std::string, sdbus::Variant>;
  using ObjectInterfaces = std::map<std::string, InterfaceProps>;
  using ManagedObjects = std::map<sdbus::ObjectPath, ObjectInterfaces>;

  template <typename T> std::optional<T> variantGet(const sdbus::Variant& value) {
    try {
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

  void mergeModemProps(const InterfaceProps& props, CellularModemInfo& out) {
    // Manufacturer and model arrive together in the initial seed; either alone is
    // still a better label than none. They are static hardware info and never show
    // up in PropertiesChanged without the other.
    std::string manufacturer;
    std::string model;
    if (auto it = props.find("Manufacturer"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        manufacturer = std::move(*v);
      }
    }
    if (auto it = props.find("Model"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        model = std::move(*v);
      }
    }
    if (!manufacturer.empty() || !model.empty()) {
      out.name = std::move(manufacturer);
      if (!model.empty()) {
        if (!out.name.empty()) {
          out.name += ' ';
        }
        out.name += model;
      }
    }
    if (auto it = props.find("State"); it != props.end()) {
      if (auto v = variantGet<std::int32_t>(it->second)) {
        out.state = static_cast<CellularModemState>(*v);
      }
    }
    if (auto it = props.find("AccessTechnologies"); it != props.end()) {
      if (auto v = variantGet<std::uint32_t>(it->second)) {
        out.accessTechnologies = *v;
      }
    }
    if (auto it = props.find("SignalQuality"); it != props.end()) {
      // (ub): percent plus a "recent" flag; the percent is all the UI needs.
      if (auto v = variantGet<sdbus::Struct<std::uint32_t, bool>>(it->second)) {
        out.signalQuality = static_cast<std::uint8_t>(std::min(v->get<0>(), 100U));
      }
    }
  }

  void merge3gppProps(const InterfaceProps& props, CellularModemInfo& out) {
    if (auto it = props.find("OperatorName"); it != props.end()) {
      if (auto v = variantGet<std::string>(it->second)) {
        out.operatorName = std::move(*v);
      }
    }
  }

} // namespace

struct ModemManagerService::Impl {
  ModemManagerService& self;
  SystemBus& bus;
  std::unique_ptr<sdbus::IProxy> daemon; // NameOwnerChanged watch on the bus daemon.
  std::unique_ptr<sdbus::IProxy> root;   // ObjectManager on "/"
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> modemProxies;

  Impl(ModemManagerService& s, SystemBus& b) : self(s), bus(b) {}

  CellularModemInfo* findModem(const std::string& path) {
    auto it = std::ranges::find(self.m_modems, path, &CellularModemInfo::path);
    return it != self.m_modems.end() ? &*it : nullptr;
  }

  void ensureModemProxy(const std::string& path) {
    if (modemProxies.contains(path)) {
      return;
    }
    std::unique_ptr<sdbus::IProxy> proxy;
    try {
      proxy = sdbus::createProxy(bus.connection(), kMmBusName, sdbus::ObjectPath{path});
    } catch (const sdbus::Error& e) {
      kLog.debug("proxy create failed {}: {}", path, e.what());
      return;
    }
    proxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this, path](
                  const std::string& interfaceName, const InterfaceProps& changed,
                  const std::vector<std::string>& /*invalidated*/
              ) { onPropertiesChanged(path, interfaceName, changed); });
    modemProxies.emplace(path, std::move(proxy));
  }

  void adoptModem(const sdbus::ObjectPath& path, const ObjectInterfaces& interfaces) {
    CellularModemInfo info;
    if (auto* existing = findModem(path)) {
      info = *existing;
    } else {
      info.path = path;
    }
    if (auto it = interfaces.find(kModemInterface); it != interfaces.end()) {
      mergeModemProps(it->second, info);
    }
    if (auto it = interfaces.find(kModem3gppInterface); it != interfaces.end()) {
      merge3gppProps(it->second, info);
    }
    if (auto* existing = findModem(path)) {
      *existing = std::move(info);
    } else {
      self.m_modems.push_back(std::move(info));
    }
    ensureModemProxy(path);
    enableSignalRefresh(path);
  }

  void enableSignalRefresh(const std::string& path) {
    auto it = modemProxies.find(path);
    if (it == modemProxies.end()) {
      return;
    }
    it->second->callMethodAsync("Setup")
        .onInterface(kModemSignalInterface)
        .withArguments(kSignalRefreshRateSeconds)
        .uponReplyInvoke([path](std::optional<sdbus::Error> err) {
          if (err.has_value()) {
            kLog.debug("Signal.Setup failed path={}: {}", path, err->what());
          }
        });
  }

  void onInterfacesAdded(const sdbus::ObjectPath& path, const ObjectInterfaces& interfaces) {
    if (!interfaces.contains(kModemInterface)) {
      return;
    }
    adoptModem(path, interfaces);
    self.emitChanged();
  }

  void onInterfacesRemoved(const sdbus::ObjectPath& path, const std::vector<std::string>& interfaces) {
    if (std::ranges::find(interfaces, kModemInterface) == interfaces.end()) {
      return;
    }
    const std::string& pathStr = path;
    std::erase_if(self.m_modems, [&](const CellularModemInfo& m) { return m.path == pathStr; });
    modemProxies.erase(pathStr);
    self.emitChanged();
  }

  void
  onPropertiesChanged(const std::string& objectPath, const std::string& interfaceName, const InterfaceProps& changed) {
    auto* modem = findModem(objectPath);
    if (modem == nullptr) {
      return;
    }
    const bool wasEnabled = modem->enabled();
    CellularModemInfo updated = *modem;
    if (interfaceName == kModemInterface) {
      mergeModemProps(changed, updated);
    } else if (interfaceName == kModem3gppInterface) {
      merge3gppProps(changed, updated);
    } else {
      return;
    }
    if (updated != *modem) {
      if (!wasEnabled && updated.enabled()) {
        enableSignalRefresh(objectPath);
      }
      *modem = std::move(updated);
      self.emitChanged();
    }
  }

  void seedFromManagedObjects(const ManagedObjects& objects) {
    for (const auto& [path, interfaces] : objects) {
      if (interfaces.contains(kModemInterface)) {
        adoptModem(path, interfaces);
      }
    }
  }

  // Subscribe to the running daemon's ObjectManager and pull the current modem set.
  void attach() {
    if (root != nullptr) {
      return;
    }
    root = sdbus::createProxy(bus.connection(), kMmBusName, kRootPath);
    root->uponSignal("InterfacesAdded")
        .onInterface(kObjectManagerInterface)
        .call([this](const sdbus::ObjectPath& path, const ObjectInterfaces& interfaces) {
          onInterfacesAdded(path, interfaces);
        });
    root->uponSignal("InterfacesRemoved")
        .onInterface(kObjectManagerInterface)
        .call([this](const sdbus::ObjectPath& path, const std::vector<std::string>& interfaces) {
          onInterfacesRemoved(path, interfaces);
        });
    self.refresh();
  }

  // Drop every proxy and cached modem. A killed daemon sends no InterfacesRemoved,
  // so the lost bus name is the only removal notification that is guaranteed.
  void detach() {
    root.reset();
    modemProxies.clear();
    self.m_hasStateSnapshot = false;
    if (!self.m_modems.empty()) {
      self.m_modems.clear();
      self.emitChanged();
    }
  }

  void onNameOwnerChanged(const std::string& newOwner) {
    if (newOwner.empty()) {
      kLog.info("modem manager left the bus");
      detach();
      return;
    }
    // A restart hands the name directly from the old owner to the new one, so a
    // non-empty owner means (re)attach even when already attached.
    kLog.info("modem manager appeared on the bus");
    detach();
    attach();
  }
};

ModemManagerService::ModemManagerService(SystemBus& bus) : m_impl(std::make_unique<Impl>(*this, bus)) {
  // ModemManager is D-Bus-activated and not tied to the shell's lifetime: it starts
  // on demand and can die mid-session, e.g. killed while hanging on an unresponsive
  // modem port. Follow the bus name instead of fixing availability at construction,
  // or a shell started while it is down would never offer cellular controls.
  m_impl->daemon = sdbus::createProxy(bus.connection(), kDaemonBusName, kDaemonPath);
  m_impl->daemon->uponSignal("NameOwnerChanged")
      .onInterface(kDaemonInterface)
      .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
        if (name != kMmBusName) {
          return;
        }
        m_impl->onNameOwnerChanged(newOwner);
      });

  if (bus.nameHasOwner(kMmBusName)) {
    m_impl->attach();
  } else {
    kLog.info("modem manager not on the bus; waiting for it to appear");
  }
}

ModemManagerService::~ModemManagerService() = default;

void ModemManagerService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ModemManagerService::refresh() {
  if (m_impl == nullptr || m_impl->root == nullptr) {
    return;
  }
  m_impl->root->callMethodAsync("GetManagedObjects")
      .onInterface(kObjectManagerInterface)
      .uponReplyInvoke([this](std::optional<sdbus::Error> err, ManagedObjects objects) {
        if (err.has_value()) {
          kLog.debug("GetManagedObjects failed: {}", err->what());
          return;
        }
        m_modems.clear();
        m_impl->modemProxies.clear();
        m_impl->seedFromManagedObjects(objects);
        m_hasStateSnapshot = true;
        emitChanged();
      });
}

const CellularModemInfo* ModemManagerService::primaryModem() const noexcept {
  if (m_modems.empty()) {
    return nullptr;
  }
  for (const auto& modem : m_modems) {
    if (modem.connected()) {
      return &modem;
    }
  }
  for (const auto& modem : m_modems) {
    if (modem.enabled()) {
      return &modem;
    }
  }
  return &m_modems.front();
}

void ModemManagerService::setModemEnabled(const std::string& modemPath, bool enabled) {
  auto it = m_impl->modemProxies.find(modemPath);
  if (it == m_impl->modemProxies.end()) {
    return;
  }
  // Async: the call is polkit-gated, and a sync call can stall the main loop
  // while authorization is pending.
  it->second->callMethodAsync("Enable")
      .onInterface(kModemInterface)
      .withArguments(enabled)
      .uponReplyInvoke([modemPath, enabled](std::optional<sdbus::Error> err) {
        if (err.has_value()) {
          kLog.warn("Enable({}) failed path={}: {}", enabled, modemPath, err->what());
        }
      });
}

void ModemManagerService::setAllModemsEnabled(bool enabled) {
  for (const auto& modem : m_modems) {
    if (modem.enabled() != enabled) {
      setModemEnabled(modem.path, enabled);
    }
  }
}

void ModemManagerService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

const char* cellularAccessTechnologyName(std::uint32_t accessTechnologies) noexcept {
  // MMModemAccessTechnology bits.
  constexpr std::uint32_t kGsm = (1U << 1) | (1U << 2);
  constexpr std::uint32_t kGprs = 1U << 3;
  constexpr std::uint32_t kEdge = 1U << 4;
  constexpr std::uint32_t kUmts = 1U << 5;
  constexpr std::uint32_t kHspa = (1U << 6) | (1U << 7) | (1U << 8);
  constexpr std::uint32_t kHspaPlus = 1U << 9;
  constexpr std::uint32_t k1xrtt = 1U << 10;
  constexpr std::uint32_t kEvdo = (1U << 11) | (1U << 12) | (1U << 13);
  constexpr std::uint32_t kLte = (1U << 14) | (1U << 16) | (1U << 17); // incl. Cat-M / NB-IoT
  constexpr std::uint32_t k5gnr = 1U << 15;

  const std::uint32_t tech = accessTechnologies;
  if ((tech & k5gnr) != 0U) {
    return "5G";
  }
  if ((tech & kLte) != 0U) {
    return "LTE";
  }
  if ((tech & kHspaPlus) != 0U) {
    return "HSPA+";
  }
  if ((tech & kHspa) != 0U) {
    return "HSPA";
  }
  if ((tech & kUmts) != 0U) {
    return "UMTS";
  }
  if ((tech & kEvdo) != 0U) {
    return "EV-DO";
  }
  if ((tech & k1xrtt) != 0U) {
    return "1xRTT";
  }
  if ((tech & kEdge) != 0U) {
    return "EDGE";
  }
  if ((tech & kGprs) != 0U) {
    return "GPRS";
  }
  if ((tech & kGsm) != 0U) {
    return "GSM";
  }
  return "";
}

std::string cellularStateText(CellularModemState state) {
  switch (state) {
  case CellularModemState::Disabled:
  case CellularModemState::Disabling:
    return i18n::tr("control-center.network.cellular-off");
  case CellularModemState::Enabling:
    return i18n::tr("control-center.network.cellular-enabling");
  case CellularModemState::Enabled:
    return i18n::tr("control-center.network.cellular-no-service");
  case CellularModemState::Searching:
    return i18n::tr("control-center.network.cellular-searching");
  case CellularModemState::Registered:
    return i18n::tr("control-center.network.cellular-registered");
  case CellularModemState::Disconnecting:
  case CellularModemState::Connecting:
    return i18n::tr("control-center.network.cellular-connecting");
  case CellularModemState::Connected:
    return i18n::tr("control-center.network.cellular-connected");
  case CellularModemState::Locked:
    return i18n::tr("control-center.network.cellular-locked");
  case CellularModemState::Failed:
    return i18n::tr("control-center.network.cellular-failed");
  case CellularModemState::Unknown:
  case CellularModemState::Initializing:
    return i18n::tr("control-center.network.cellular-no-service");
  }
  return i18n::tr("control-center.network.cellular-no-service");
}
