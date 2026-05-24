#include "dbus/power/power_profiles_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <map>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>

std::string profileLabel(std::string_view profile) {
  if (profile == "power-saver") {
    return i18n::tr("power.profiles.power-saver");
  }
  if (profile == "balanced") {
    return i18n::tr("power.profiles.balanced");
  }
  if (profile == "performance") {
    return i18n::tr("power.profiles.performance");
  }
  return std::string(profile);
}

namespace {

  constexpr Logger kLog("power");

  static const sdbus::ServiceName kPowerProfilesBusName{"org.freedesktop.UPower.PowerProfiles"};
  static const sdbus::ObjectPath kPowerProfilesObjectPath{"/org/freedesktop/UPower/PowerProfiles"};
  static constexpr auto kPowerProfilesInterface = "org.freedesktop.UPower.PowerProfiles";
  static constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  template <typename T> T getPropertyOr(sdbus::IProxy& proxy, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(kPowerProfilesInterface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  std::vector<std::string> decodeProfiles(const sdbus::Variant& value) {
    std::vector<std::string> profiles;

    try {
      const auto profileMaps = value.get<std::vector<std::map<std::string, sdbus::Variant>>>();
      profiles.reserve(profileMaps.size());
      for (const auto& profileMap : profileMaps) {
        auto it = profileMap.find("Profile");
        if (it == profileMap.end()) {
          continue;
        }
        try {
          const std::string profile = it->second.get<std::string>();
          if (!profile.empty()) {
            profiles.push_back(profile);
          }
        } catch (const sdbus::Error&) {
        }
      }
    } catch (const sdbus::Error&) {
    }

    std::ranges::sort(profiles);
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
    return profiles;
  }

} // namespace

std::string_view profileGlyphName(std::string_view profile) {
  if (profile == "performance") {
    return "performance";
  }
  if (profile == "power-saver") {
    return "powersaver";
  }
  return "balanced";
}

PowerProfilesService::PowerProfilesService(SystemBus& bus) : m_bus(bus) {
  m_proxy = sdbus::createProxy(m_bus.connection(), kPowerProfilesBusName, kPowerProfilesObjectPath);

  m_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                const std::vector<std::string>& invalidatedProperties
            ) {
        if (interfaceName != kPowerProfilesInterface) {
          return;
        }

        bool relevant = changedProperties.contains("ActiveProfile") || changedProperties.contains("Profiles") ||
                        changedProperties.contains("PerformanceInhibited");

        if (!relevant) {
          relevant = std::ranges::find(invalidatedProperties, "ActiveProfile") != invalidatedProperties.end() ||
                     std::ranges::find(invalidatedProperties, "Profiles") != invalidatedProperties.end() ||
                     std::ranges::find(invalidatedProperties, "PerformanceInhibited") != invalidatedProperties.end();
        }

        if (relevant) {
          refresh();
        }
      });

  refresh();
}

void PowerProfilesService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void PowerProfilesService::refresh() { emitChangedIfNeeded(readState()); }

bool PowerProfilesService::setActiveProfile(std::string_view profile) {
  if (profile.empty()) {
    return false;
  }

  const std::string requested(profile);
  if (requested != m_state.activeProfile) {
    m_pendingLocalActiveProfile = requested;
  }
  try {
    m_proxy->setProperty("ActiveProfile").onInterface(kPowerProfilesInterface).toValue(requested);
    refresh();
    return true;
  } catch (const sdbus::Error& e) {
    if (m_pendingLocalActiveProfile.has_value() && *m_pendingLocalActiveProfile == requested) {
      m_pendingLocalActiveProfile.reset();
    }
    kLog.warn("power profile change failed profile={} err={}", requested, e.what());
    return false;
  }
}

bool PowerProfilesService::cycleActiveProfile() {
  const auto& profs = profiles();
  if (profs.empty()) {
    return false;
  }
  const std::string& current = activeProfile();
  auto it = std::find(profs.begin(), profs.end(), current);
  if (it == profs.end()) {
    return setActiveProfile(profs.front());
  }
  ++it;
  if (it == profs.end()) {
    it = profs.begin();
  }
  return setActiveProfile(*it);
}

PowerProfilesState PowerProfilesService::readState() const {
  PowerProfilesState next;
  next.activeProfile = getPropertyOr<std::string>(*m_proxy, "ActiveProfile", "");
  next.performanceInhibited = getPropertyOr<std::string>(*m_proxy, "PerformanceInhibited", "");

  try {
    const sdbus::Variant profilesVariant = m_proxy->getProperty("Profiles").onInterface(kPowerProfilesInterface);
    next.profiles = decodeProfiles(profilesVariant);
  } catch (const sdbus::Error&) {
    next.profiles.clear();
  }

  return next;
}

PowerProfilesChangeOrigin PowerProfilesService::consumeActiveProfileChangeOrigin(std::string_view profile) {
  if (!m_pendingLocalActiveProfile.has_value()) {
    return PowerProfilesChangeOrigin::External;
  }
  const bool matchesLocalRequest = *m_pendingLocalActiveProfile == profile;
  m_pendingLocalActiveProfile.reset();
  return matchesLocalRequest ? PowerProfilesChangeOrigin::Noctalia : PowerProfilesChangeOrigin::External;
}

void PowerProfilesService::emitChangedIfNeeded(const PowerProfilesState& next) {
  if (next == m_state) {
    return;
  }

  const bool activeProfileChanged = next.activeProfile != m_state.activeProfile;
  const PowerProfilesChangeOrigin origin =
      activeProfileChanged ? consumeActiveProfileChangeOrigin(next.activeProfile) : PowerProfilesChangeOrigin::External;
  m_state = next;
  if (m_changeCallback) {
    m_changeCallback(m_state, origin);
  }
}

void PowerProfilesService::registerIpc(IpcService& ipc) {
  ipc.registerHandler(
      "power-set",
      [this](const std::string& args) -> std::string {
        const std::string profile = StringUtils::trim(args);
        if (profile.empty()) {
          return "error: profile required (power-set <profile>); typical values: performance, balanced, "
                 "power-saver\n";
        }
        const auto& available = profiles();
        if (!available.empty()) {
          if (std::find(available.begin(), available.end(), profile) == available.end()) {
            std::string suffix = "; available:";
            for (std::size_t i = 0; i < available.size(); ++i) {
              suffix.push_back(' ');
              suffix += available[i];
            }
            suffix.push_back('\n');
            return "error: unknown profile \"" + profile + "\"" + suffix;
          }
        }
        if (!setActiveProfile(profile)) {
          return "error: failed to set power profile\n";
        }
        return "ok\n";
      },
      "power-set <profile>", "Set the UPower power profile (e.g. performance, balanced, power-saver)"
  );
  ipc.registerHandler(
      "power-cycle",
      [this](const std::string& args) -> std::string {
        if (!StringUtils::trim(args).empty()) {
          return "error: power-cycle takes no arguments\n";
        }
        if (!cycleActiveProfile()) {
          return "error: could not cycle power profile (no profiles from UPower or set failed)\n";
        }
        return "ok\n";
      },
      "power-cycle", "Switch to the next power profile in UPower's ordered list (wraps)"
  );
}
