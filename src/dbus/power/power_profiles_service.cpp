#include "dbus/power/power_profiles_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <span>
#include <vector>

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

  const sdbus::ServiceName kPowerProfilesBusName{"org.freedesktop.UPower.PowerProfiles"};
  const sdbus::ObjectPath kPowerProfilesObjectPath{"/org/freedesktop/UPower/PowerProfiles"};
  constexpr auto kPowerProfilesInterface = "org.freedesktop.UPower.PowerProfiles";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  using VariantMap = std::map<std::string, sdbus::Variant>;

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
          const auto profile = it->second.get<std::string>();
          if (!profile.empty()) {
            profiles.push_back(profile);
          }
        } catch (const sdbus::Error&) {
        }
      }
    } catch (const sdbus::Error&) {
    }

    std::ranges::sort(profiles);
    profiles.erase(std::ranges::unique(profiles).begin(), profiles.end());
    return profiles;
  }

  std::string stringProperty(const VariantMap& properties, std::string_view name) {
    const auto it = properties.find(std::string{name});
    if (it == properties.end()) {
      return {};
    }

    try {
      return it->second.get<std::string>();
    } catch (const sdbus::Error&) {
      return {};
    }
  }

  PowerProfilesState decodeState(const VariantMap& properties) {
    PowerProfilesState next;
    next.activeProfile = stringProperty(properties, "ActiveProfile");
    next.performanceInhibited = stringProperty(properties, "PerformanceInhibited");

    const auto profilesIt = properties.find("Profiles");
    if (profilesIt != properties.end()) {
      next.profiles = decodeProfiles(profilesIt->second);
    }

    return next;
  }

  bool isTimeoutError(const sdbus::Error& error) {
    const auto& name = error.getName();
    return (
        name == sdbus::Error::Name{"org.freedesktop.DBus.Error.NoReply"}
        || name == sdbus::Error::Name{"org.freedesktop.DBus.Error.Timeout"}
        || name == sdbus::Error::Name{"org.freedesktop.DBus.Error.TimedOut"}
    );
  }

  void appendCanonicalCycleSequence(std::vector<std::string_view>& seq) {
    for (const auto& candidate : powerProfileOrder()) {
      seq.push_back(candidate);
    }
  }

  std::vector<std::string_view>
  cycleSequenceFor(const std::vector<std::string>& profs, std::string_view activeProfile) {
    std::vector<std::string_view> seq;
    if (!profs.empty()) {
      for (const auto& candidate : powerProfileOrder()) {
        if (std::ranges::contains(profs, candidate)) {
          seq.push_back(candidate);
        }
      }
      if (seq.empty()) {
        seq.assign(profs.begin(), profs.end());
      }
      return seq;
    }
    if (!activeProfile.empty()) {
      appendCanonicalCycleSequence(seq);
    }
    return seq;
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

std::span<const std::string_view> powerProfileOrder() {
  static constexpr std::array<std::string_view, 3> kOrder = {"power-saver", "balanced", "performance"};
  return kOrder;
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

        bool relevant = changedProperties.contains("ActiveProfile")
            || changedProperties.contains("Profiles")
            || changedProperties.contains("PerformanceInhibited");

        if (!relevant) {
          relevant = std::ranges::contains(invalidatedProperties, "ActiveProfile")
              || std::ranges::contains(invalidatedProperties, "Profiles")
              || std::ranges::contains(invalidatedProperties, "PerformanceInhibited");
        }

        if (relevant) {
          refresh();
        }
      });

  refresh();
}

PowerProfilesService::~PowerProfilesService() { m_lifetimeToken.reset(); }

void PowerProfilesService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void PowerProfilesService::refresh() {
  if (m_refreshInFlight) {
    m_refreshQueued = true;
    return;
  }

  m_refreshInFlight = true;
  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;

  try {
    m_proxy->callMethodAsync("GetAll")
        .onInterface(kPropertiesInterface)
        .withArguments(std::string{kPowerProfilesInterface})
        .uponReplyInvoke([this, lifetimeToken](std::optional<sdbus::Error> err, VariantMap properties) {
          if (lifetimeToken.expired()) {
            return;
          }

          if (err.has_value()) {
            kLog.debug("power profiles refresh failed: {}", err->what());
            if (!m_hasStateSnapshot && isTimeoutError(*err)) {
              m_refreshQueued = true;
            }
          } else {
            emitChangedIfNeeded(decodeState(properties), true);
          }

          m_refreshInFlight = false;
          if (m_refreshQueued) {
            m_refreshQueued = false;
            refresh();
          }
        });
  } catch (const sdbus::Error& e) {
    kLog.debug("power profiles refresh dispatch failed: {}", e.what());
    m_refreshInFlight = false;
    if (m_refreshQueued) {
      m_refreshQueued = false;
      refresh();
    }
  }
}

bool PowerProfilesService::setActiveProfile(std::string_view profile) {
  if (profile.empty()) {
    return false;
  }

  const std::string requested(profile);
  if (requested != m_state.activeProfile) {
    m_pendingLocalActiveProfile = requested;
  }

  const std::weak_ptr<int> lifetimeToken = m_lifetimeToken;
  try {
    m_proxy->setPropertyAsync("ActiveProfile")
        .onInterface(kPowerProfilesInterface)
        .toValue(requested)
        .uponReplyInvoke([lifetimeToken, requested](std::optional<sdbus::Error> err) {
          if (lifetimeToken.expired() || !err.has_value()) {
            return;
          }
          // The optimistic update below already reconciles via refresh(); just surface the failure.
          kLog.warn("power profile change failed profile={} err={}", requested, err->what());
        });
  } catch (const sdbus::Error& e) {
    if (m_pendingLocalActiveProfile.has_value() && *m_pendingLocalActiveProfile == requested) {
      m_pendingLocalActiveProfile.reset();
    }
    kLog.warn("power profile change dispatch failed profile={} err={}", requested, e.what());
    return false;
  }

  PowerProfilesState next = m_state;
  next.activeProfile = requested;
  emitChangedIfNeeded(std::move(next), false);
  refresh();
  return true;
}

bool PowerProfilesService::cycleActiveProfile(int direction) {
  const std::vector<std::string_view> seq = cycleSequenceFor(profiles(), activeProfile());
  if (seq.empty()) {
    return false;
  }

  const long n = static_cast<long>(seq.size());
  const int step = direction >= 0 ? 1 : -1;
  const auto it = std::ranges::find(seq, activeProfile());
  if (it == seq.end()) {
    // Current profile unknown: land on the end matching the requested direction.
    return setActiveProfile(seq[step > 0 ? 0U : static_cast<std::size_t>(n - 1)]);
  }
  long target = std::distance(seq.begin(), it) + step;
  target %= n;
  if (target < 0) {
    target += n;
  }
  return setActiveProfile(seq[static_cast<std::size_t>(target)]);
}

PowerProfilesChangeOrigin PowerProfilesService::consumeActiveProfileChangeOrigin(std::string_view profile) {
  if (!m_pendingLocalActiveProfile.has_value()) {
    return PowerProfilesChangeOrigin::External;
  }
  const bool matchesLocalRequest = *m_pendingLocalActiveProfile == profile;
  m_pendingLocalActiveProfile.reset();
  return matchesLocalRequest ? PowerProfilesChangeOrigin::Noctalia : PowerProfilesChangeOrigin::External;
}

void PowerProfilesService::emitChangedIfNeeded(PowerProfilesState next, bool stateSnapshot) {
  if (stateSnapshot && next.profiles.empty() && !m_state.profiles.empty()) {
    next.profiles = m_state.profiles;
  }

  const bool firstSnapshot = stateSnapshot && !m_hasStateSnapshot;
  const bool stateChanged = next != m_state;
  const bool activeProfileChanged = next.activeProfile != m_state.activeProfile;
  const PowerProfilesChangeOrigin origin =
      activeProfileChanged ? consumeActiveProfileChangeOrigin(next.activeProfile) : PowerProfilesChangeOrigin::External;

  m_state = std::move(next);
  if (stateSnapshot) {
    m_hasStateSnapshot = true;
  }

  if ((firstSnapshot || stateChanged) && m_changeCallback) {
    m_changeCallback(m_state, origin);
  }
}

void PowerProfilesService::registerIpc(IpcService& ipc, StateFeedbackCallback stateFeedback) {
  ipc.registerHandler(
      "power-set",
      [this, stateFeedback](const std::string& args) -> std::string {
        const std::string profile = StringUtils::trim(args);
        if (profile.empty()) {
          return "error: profile required (power-set <profile>); typical values: performance, balanced, "
                 "power-saver\n";
        }
        const auto& available = profiles();
        if (!available.empty()) {
          if (!std::ranges::contains(available, profile)) {
            std::string suffix = "; available:";
            for (const auto& availableProfile : available) {
              suffix.push_back(' ');
              suffix += availableProfile;
            }
            suffix.push_back('\n');
            return "error: unknown profile \"" + profile + "\"" + suffix;
          }
        }
        const std::string previous = activeProfile();
        if (!setActiveProfile(profile)) {
          return "error: failed to set power profile\n";
        }
        if (stateFeedback && previous != activeProfile() && !activeProfile().empty()) {
          stateFeedback(activeProfile());
        }
        return "ok\n";
      },
      "power-set <profile>", "Set the UPower power profile (e.g. performance, balanced, power-saver)"
  );
  ipc.registerHandler(
      "power-cycle",
      [this, stateFeedback](const std::string& args) -> std::string {
        if (!StringUtils::trim(args).empty()) {
          return "error: power-cycle takes no arguments\n";
        }
        const std::string previous = activeProfile();
        if (!cycleActiveProfile()) {
          if (profiles().empty() && activeProfile().empty()) {
            return "error: could not cycle power profile (UPower profile list not loaded)\n";
          }
          return "error: could not cycle power profile (set failed)\n";
        }
        if (stateFeedback && previous != activeProfile() && !activeProfile().empty()) {
          stateFeedback(activeProfile());
        }
        return "ok\n";
      },
      "power-cycle", "Switch to the next power profile in UPower's ordered list (wraps)"
  );
}
