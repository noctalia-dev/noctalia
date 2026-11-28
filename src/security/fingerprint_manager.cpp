#include "security/fingerprint_manager.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/system_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <utility>

namespace {
  constexpr Logger kLog("fingerprint-manager");

  const sdbus::ServiceName kFprintBusName{"net.reactivated.Fprint"};
  const sdbus::ObjectPath kManagerPath{"/net/reactivated/Fprint/Manager"};
  constexpr auto kManagerInterface = "net.reactivated.Fprint.Manager";
  constexpr auto kDeviceInterface = "net.reactivated.Fprint.Device";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kNoEnrolledPrints = "net.reactivated.Fprint.Error.NoEnrolledPrints";
  constexpr auto kNoSuchDevice = "net.reactivated.Fprint.Error.NoSuchDevice";

  // Claim and EnrollStart may wait on a polkit prompt; the default 25 s D-Bus timeout is too short.
  constexpr auto kInteractiveTimeout = std::chrono::minutes(2);
  constexpr auto kVerifyTimeout = std::chrono::seconds(30);

  using fingerprint::ScanResult;

  std::string errorMessage(const sdbus::Error& error) {
    static const std::map<std::string, std::string, std::less<>> kErrors = {
        {"net.reactivated.Fprint.Error.PermissionDenied", "settings.fingerprint.error.permission-denied"},
        {"net.reactivated.Fprint.Error.AlreadyInUse", "settings.fingerprint.error.in-use"},
        {kNoEnrolledPrints, "settings.fingerprint.error.no-prints"},
        {"net.reactivated.Fprint.Error.PrintsNotDeleted", "settings.fingerprint.error.not-deleted"},
        {kNoSuchDevice, "settings.fingerprint.error.no-device"},
    };
    const auto it = kErrors.find(std::string_view(error.getName()));
    if (it != kErrors.end()) {
      return i18n::tr(it->second);
    }
    return i18n::tr("settings.fingerprint.error.generic", "message", error.getMessage());
  }

  // Status line for a scan result; the caller handles Match itself (it may name the finger).
  std::pair<const char*, bool> scanMessage(ScanResult result) {
    switch (result) {
    case ScanResult::StagePassed:
      return {"settings.fingerprint.status.stage-passed", false};
    case ScanResult::Completed:
      return {"settings.fingerprint.status.enroll-completed", false};
    case ScanResult::Match:
      return {"settings.fingerprint.status.match", false};
    case ScanResult::RetryScan:
      return {"settings.fingerprint.status.retry-scan", false};
    case ScanResult::SwipeTooShort:
      return {"settings.fingerprint.status.swipe-too-short", false};
    case ScanResult::FingerNotCentered:
      return {"settings.fingerprint.status.finger-not-centered", false};
    case ScanResult::RemoveAndRetry:
      return {"settings.fingerprint.status.remove-and-retry", false};
    case ScanResult::NoMatch:
      return {"settings.fingerprint.status.no-match", true};
    case ScanResult::Duplicate:
      return {"settings.fingerprint.status.duplicate", true};
    case ScanResult::DataFull:
      return {"settings.fingerprint.status.data-full", true};
    case ScanResult::Disconnected:
      return {"settings.fingerprint.status.disconnected", true};
    case ScanResult::Failed:
      break;
    }
    return {nullptr, true};
  }

  template <typename T>
  T variantOr(const std::map<std::string, sdbus::Variant>& properties, const std::string& name, T fallback) {
    const auto it = properties.find(name);
    if (it == properties.end()) {
      return fallback;
    }
    try {
      return it->second.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  void ignoreReply(std::optional<sdbus::Error> /*error*/) {}
} // namespace

FingerprintManager::FingerprintManager(SystemBus& bus) : m_bus(bus) {}

FingerprintManager::~FingerprintManager() {
  m_alive.reset();
  m_verifyTimeout.stop();
  if (m_device != nullptr && m_claimed) {
    try {
      m_device->callMethod("Release").onInterface(kDeviceInterface);
    } catch (const sdbus::Error&) {
    }
  }
}

void FingerprintManager::setChangedCallback(ChangedCallback callback) { m_onChanged = std::move(callback); }

void FingerprintManager::later(std::function<void()> call) {
  DeferredCall::callLater([alive = std::weak_ptr<void>(m_alive), call = std::move(call)] {
    if (!alive.expired()) {
      call();
    }
  });
}

void FingerprintManager::refresh() {
  if (m_loading || busy()) {
    return;
  }
  m_loading = true;
  try {
    if (m_manager == nullptr) {
      m_manager = sdbus::createProxy(m_bus.connection(), kFprintBusName, kManagerPath);
    }
    // GetDevices D-Bus-activates fprintd, which can take a moment; without fprintd it fails
    // right away and the list stays empty.
    m_manager->callMethodAsync("GetDevices")
        .onInterface(kManagerInterface)
        .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive)](
                             std::optional<sdbus::Error> e, std::vector<sdbus::ObjectPath> paths
                         ) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            kLog.debug("no fingerprint readers: {}", e->what());
            paths.clear();
          }
          readDevices(std::move(paths));
        });
  } catch (const sdbus::Error& e) {
    kLog.warn("could not list fingerprint readers: {}", e.what());
    readDevices({});
  }
}

void FingerprintManager::readDevices(std::vector<sdbus::ObjectPath> paths) {
  // One GetAll per reader, all in flight at once; the last reply publishes the list.
  struct Probe {
    std::vector<FingerprintDevice> devices;
    std::vector<std::unique_ptr<sdbus::IProxy>> proxies;
    std::size_t pending = 0;
  };
  auto probe = std::make_shared<Probe>();
  const auto publish = [this, probe] {
    m_loading = false;
    m_loaded = true;
    m_devices = std::move(probe->devices);
    // The probe's proxies own the reply handlers holding it: drop them outside those handlers.
    DeferredCall::callLater([probe] { probe->proxies.clear(); });

    if (std::ranges::find(m_devices, m_selectedPath, &FingerprintDevice::path) == m_devices.end()) {
      m_selectedPath = m_devices.empty() ? std::string{} : m_devices.front().path;
      m_enrolled.clear();
      dropDeviceProxyLater();
    }
    if (m_selectedPath.empty()) {
      notifyChanged();
      return;
    }
    // After the dropped proxy is gone, list on a fresh one.
    later([this] { loadEnrolled(); });
  };

  probe->devices.reserve(paths.size());
  for (const auto& path : paths) {
    probe->devices.push_back(FingerprintDevice{.path = path, .name = path});
  }
  probe->pending = probe->devices.size();
  if (probe->pending == 0) {
    publish();
    return;
  }
  for (std::size_t i = 0; i < probe->devices.size(); ++i) {
    const auto done = [probe, publish] {
      if (--probe->pending == 0) {
        publish();
      }
    };
    try {
      auto proxy = sdbus::createProxy(m_bus.connection(), kFprintBusName, sdbus::ObjectPath{probe->devices[i].path});
      proxy->callMethodAsync("GetAll")
          .onInterface(kPropertiesInterface)
          .withArguments(std::string{kDeviceInterface})
          .uponReplyInvoke([alive = std::weak_ptr<void>(m_alive), probe, i, done](
                               std::optional<sdbus::Error> e, const std::map<std::string, sdbus::Variant>& properties
                           ) {
            if (alive.expired()) {
              return;
            }
            if (e.has_value()) {
              kLog.debug("could not read fingerprint reader properties: {}", e->what());
            } else {
              auto& device = probe->devices[i];
              device.name = variantOr<std::string>(properties, "name", device.name);
              device.scanType = variantOr<std::string>(properties, "scan-type", device.scanType);
              device.enrollStages = variantOr<std::int32_t>(properties, "num-enroll-stages", -1);
            }
            done();
          });
      probe->proxies.push_back(std::move(proxy));
    } catch (const sdbus::Error& e) {
      kLog.debug("could not read fingerprint reader properties: {}", e.what());
      done();
    }
  }
}

void FingerprintManager::selectDevice(const std::string& path) {
  if (path == m_selectedPath || busy()) {
    return;
  }
  m_selectedPath = path;
  m_enrolled.clear();
  clearStatus();
  dropDeviceProxyLater();
  later([this] { loadEnrolled(); });
  notifyChanged();
}

const FingerprintDevice* FingerprintManager::selected() const {
  const auto it = std::ranges::find(m_devices, m_selectedPath, &FingerprintDevice::path);
  return it == m_devices.end() ? nullptr : &*it;
}

int FingerprintManager::enrollStagesTotal() const noexcept {
  const auto* device = selected();
  return device == nullptr ? -1 : device->enrollStages;
}

bool FingerprintManager::ensureDeviceProxy() {
  if (m_device != nullptr) {
    return true;
  }
  if (m_selectedPath.empty()) {
    return false;
  }
  try {
    m_device = sdbus::createProxy(m_bus.connection(), kFprintBusName, sdbus::ObjectPath{m_selectedPath});
  } catch (const sdbus::Error& e) {
    kLog.warn("could not create fingerprint reader proxy: {}", e.what());
    return false;
  }
  m_device->uponSignal("EnrollStatus").onInterface(kDeviceInterface).call([this](const std::string& result, bool done) {
    handleScanStatus(Mode::Enrolling, result, done);
  });
  m_device->uponSignal("VerifyStatus").onInterface(kDeviceInterface).call([this](const std::string& result, bool done) {
    handleScanStatus(Mode::Verifying, result, done);
  });
  // Names the finger being verified when fprintd checks a single one ("any" otherwise).
  m_device->uponSignal("VerifyFingerSelected").onInterface(kDeviceInterface).call([this](const std::string& finger) {
    if (m_mode == Mode::Verifying) {
      m_matchedFinger = finger;
    }
  });
  return true;
}

void FingerprintManager::dropDeviceProxyLater() {
  if (m_device == nullptr) {
    return;
  }
  // Never destroy a proxy from inside one of its own handlers.
  later([this] {
    if (!busy()) {
      m_device.reset();
      m_claimed = false;
    }
  });
}

void FingerprintManager::loadEnrolled() {
  if (!ensureDeviceProxy()) {
    m_enrolled.clear();
    notifyChanged();
    return;
  }
  // Empty user name: fprintd uses the caller's user.
  m_device->callMethodAsync("ListEnrolledFingers")
      .onInterface(kDeviceInterface)
      .withArguments(std::string{})
      .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive)](
                           std::optional<sdbus::Error> e, std::vector<std::string> fingers
                       ) {
        if (alive.expired()) {
          return;
        }
        if (e.has_value()) {
          if (e->getName() != sdbus::Error::Name{kNoEnrolledPrints}) {
            kLog.info("could not list enrolled fingers: {}", e->what());
          }
          fingers.clear();
        }
        m_enrolled = std::move(fingers);
        notifyChanged();
      });
}

void FingerprintManager::failOperation(std::string_view what, const sdbus::Error& error) {
  kLog.info("could not {}: {}", what, error.what());
  setStatus(errorMessage(error), true);
  finishOperation();
  if (error.getName() == sdbus::Error::Name{kNoSuchDevice}) {
    // The reader is gone: list again, which hides the page's card when it was the last one.
    later([this] { refresh(); });
  }
}

void FingerprintManager::claimThen(std::function<void()> next) {
  if (!ensureDeviceProxy()) {
    setStatus(i18n::tr("settings.fingerprint.error.no-device"), true);
    finishOperation();
    return;
  }
  if (m_claimed) {
    next();
    return;
  }
  m_claiming = true;
  m_device->callMethodAsync("Claim")
      .onInterface(kDeviceInterface)
      .withTimeout(kInteractiveTimeout)
      .withArguments(std::string{})
      .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive),
                        next = std::move(next)](std::optional<sdbus::Error> e) {
        if (alive.expired()) {
          return;
        }
        m_claiming = false;
        if (e.has_value()) {
          failOperation("claim the fingerprint reader", *e);
          return;
        }
        m_claimed = true;
        if (m_mode == Mode::Idle) {
          // Cancelled while the claim was pending.
          release();
          return;
        }
        next();
      });
}

void FingerprintManager::release() {
  if (m_device == nullptr || !m_claimed) {
    return;
  }
  m_claimed = false;
  m_device->callMethodAsync("Release").onInterface(kDeviceInterface).uponReplyInvoke(ignoreReply);
}

void FingerprintManager::finishOperation() {
  m_verifyTimeout.stop();
  m_mode = Mode::Idle;
  release();
  notifyChanged();
}

void FingerprintManager::deleteFinger(const std::string& finger) {
  if (busy()) {
    return;
  }
  clearStatus();
  m_mode = Mode::Deleting;
  claimThen([this, finger] {
    m_device->callMethodAsync("DeleteEnrolledFinger")
        .onInterface(kDeviceInterface)
        .withTimeout(kInteractiveTimeout)
        .withArguments(finger)
        .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive), finger](std::optional<sdbus::Error> e) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            failOperation("delete the fingerprint", *e);
            return;
          }
          std::erase(m_enrolled, finger);
          setStatus(i18n::tr("settings.fingerprint.status.deleted", "finger", fingerLabel(finger)), false);
          finishOperation();
        });
  });
  notifyChanged();
}

void FingerprintManager::startEnroll(const std::string& finger) {
  if (busy()) {
    return;
  }
  m_mode = Mode::Enrolling;
  m_enrollFinger = finger;
  m_stagesDone = 0;
  m_enrollCompleted = false;
  m_matchedFinger.clear();
  setStatus(i18n::tr("settings.fingerprint.status.preparing"), false);

  claimThen([this, finger] {
    m_device->callMethodAsync("EnrollStart")
        .onInterface(kDeviceInterface)
        .withTimeout(kInteractiveTimeout)
        .withArguments(finger)
        .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive)](std::optional<sdbus::Error> e) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            failOperation("start enrollment", *e);
          } else if (m_mode == Mode::Enrolling) {
            setStatus(touchPrompt(), false);
          }
        });
  });
  notifyChanged();
}

void FingerprintManager::startVerify() {
  if (busy()) {
    return;
  }
  m_mode = Mode::Verifying;
  m_matchedFinger.clear();
  m_enrollCompleted = false;
  setStatus(i18n::tr("settings.fingerprint.status.preparing"), false);

  claimThen([this] {
    m_device->callMethodAsync("VerifyStart")
        .onInterface(kDeviceInterface)
        .withTimeout(kInteractiveTimeout)
        .withArguments(std::string{"any"})
        .uponReplyInvoke([this, alive = std::weak_ptr<void>(m_alive)](std::optional<sdbus::Error> e) {
          if (alive.expired()) {
            return;
          }
          if (e.has_value()) {
            failOperation("start verification", *e);
            return;
          }
          if (m_mode != Mode::Verifying) {
            return;
          }
          setStatus(touchPrompt(), false);
          m_verifyTimeout.start(kVerifyTimeout, [this] {
            if (m_mode == Mode::Verifying) {
              cancel();
              setStatus(i18n::tr("settings.fingerprint.status.verify-timeout"), true);
            }
          });
        });
  });
  notifyChanged();
}

std::string FingerprintManager::touchPrompt() const {
  const auto* device = selected();
  return i18n::tr(
      device != nullptr && device->scanType == "swipe" ? "settings.fingerprint.status.swipe"
                                                       : "settings.fingerprint.status.touch"
  );
}

void FingerprintManager::handleScanStatus(Mode mode, const std::string& result, bool done) {
  if (m_mode != mode) {
    return;
  }
  kLog.debug("scan status: {} (done={})", result, done);
  const ScanResult scan = fingerprint::parseScanResult(result);

  if (mode == Mode::Enrolling && scan == ScanResult::StagePassed) {
    ++m_stagesDone;
  } else if (mode == Mode::Enrolling && scan == ScanResult::Completed) {
    m_enrollCompleted = true;
    m_stagesDone = std::max(m_stagesDone, enrollStagesTotal());
    if (!std::ranges::contains(m_enrolled, m_enrollFinger)) {
      m_enrolled.push_back(m_enrollFinger);
    }
  } else if (mode == Mode::Verifying && scan == ScanResult::Match) {
    if (m_matchedFinger.empty()) {
      m_matchedFinger = "any";
    }
  } else if (mode == Mode::Verifying && !fingerprint::isRetry(scan)) {
    m_matchedFinger.clear();
  }

  if (mode == Mode::Verifying && scan == ScanResult::Match && m_matchedFinger != "any") {
    setStatus(i18n::tr("settings.fingerprint.status.match-finger", "finger", fingerLabel(m_matchedFinger)), false);
  } else if (const auto [key, error] = scanMessage(scan); key != nullptr) {
    setStatus(i18n::tr(key), error);
  } else {
    setStatus(
        i18n::tr(
            mode == Mode::Enrolling ? "settings.fingerprint.status.enroll-failed"
                                    : "settings.fingerprint.status.verify-failed"
        ),
        true
    );
  }

  if (!done) {
    return;
  }
  if (m_device != nullptr) {
    m_device->callMethodAsync(mode == Mode::Enrolling ? "EnrollStop" : "VerifyStop")
        .onInterface(kDeviceInterface)
        .uponReplyInvoke(ignoreReply);
  }
  finishOperation();
  if (scan == ScanResult::Disconnected) {
    later([this] { refresh(); });
  }
}

void FingerprintManager::cancel() {
  if (m_mode == Mode::Idle && !m_claiming) {
    return;
  }
  const Mode mode = m_mode;
  m_verifyTimeout.stop();
  m_mode = Mode::Idle;
  if (m_device != nullptr && m_claimed && (mode == Mode::Enrolling || mode == Mode::Verifying)) {
    m_device->callMethodAsync(mode == Mode::Enrolling ? "EnrollStop" : "VerifyStop")
        .onInterface(kDeviceInterface)
        .uponReplyInvoke(ignoreReply);
  }
  release();
  if (mode == Mode::Enrolling && !m_enrollCompleted) {
    m_enrollFinger.clear();
  }
  clearStatus();
  notifyChanged();
}

void FingerprintManager::clearStatus() {
  m_status.clear();
  m_statusError = false;
  m_matchedFinger.clear();
}

void FingerprintManager::resetEnrollment() {
  if (m_mode == Mode::Enrolling) {
    return;
  }
  m_enrollFinger.clear();
  m_stagesDone = 0;
  m_enrollCompleted = false;
  clearStatus();
}

void FingerprintManager::setStatus(std::string message, bool error) {
  m_status = std::move(message);
  m_statusError = error;
  notifyChanged();
}

void FingerprintManager::notifyChanged() {
  if (m_onChanged) {
    m_onChanged();
  }
}

std::string FingerprintManager::fingerLabel(std::string_view finger) {
  const std::string key = fingerprint::fingerLabelKey(finger);
  return key.empty() ? std::string(finger) : i18n::tr(key);
}
