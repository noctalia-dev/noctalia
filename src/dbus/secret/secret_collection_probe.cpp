#include "dbus/secret/secret_collection_probe.h"

#include "core/log.h"
#include "dbus/session_bus.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <utility>

namespace {
  constexpr Logger kLog("secret-collection");
  const sdbus::ServiceName kSecretServiceBusName{"org.freedesktop.secrets"};
  const sdbus::ObjectPath kSecretServiceObjectPath{"/org/freedesktop/secrets"};
  constexpr auto kSecretServiceInterface = "org.freedesktop.Secret.Service";
  constexpr auto kSecretCollectionInterface = "org.freedesktop.Secret.Collection";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kProbeTimeout = std::chrono::seconds(3);
} // namespace

class SecretCollectionProbe::Impl {
public:
  Impl(SessionBus& bus, ResultCallback callback)
      : m_bus(bus), m_callback(std::move(callback)),
        m_serviceProxy(sdbus::createProxy(m_bus.connection(), kSecretServiceBusName, kSecretServiceObjectPath)) {}

  ~Impl() { m_pendingCall.cancel(); }

  void request() {
    if (m_inFlight) {
      m_rerunRequested = true;
      return;
    }

    m_inFlight = true;
    const std::uint64_t generation = m_generation;
    try {
      m_pendingCall =
          m_serviceProxy->callMethodAsync("ReadAlias")
              .onInterface(kSecretServiceInterface)
              .withTimeout(kProbeTimeout)
              .withArguments(std::string{"default"})
              .uponReplyInvoke([this, generation](std::optional<sdbus::Error> error, sdbus::ObjectPath collection) {
                onAliasReply(generation, std::move(error), collection);
              });
    } catch (const sdbus::Error& e) {
      kLog.debug("secret default collection alias probe dispatch failed: {}", e.what());
      finish(generation, false);
    }
  }

  void invalidate() {
    ++m_generation;
    m_pendingCall.cancel();
    m_pendingCall = {};
    m_inFlight = false;
    m_rerunRequested = false;
  }

private:
  [[nodiscard]] bool isCurrent(std::uint64_t generation) const { return m_inFlight && generation == m_generation; }

  void onAliasReply(std::uint64_t generation, std::optional<sdbus::Error> error, const sdbus::ObjectPath& collection) {
    if (!isCurrent(generation)) {
      return;
    }
    if (error.has_value()) {
      kLog.debug("secret default collection alias probe failed: {}", error->what());
      finish(generation, false);
      return;
    }

    const std::string& collectionPath = collection;
    if (collectionPath.empty() || collectionPath == "/") {
      finish(generation, false);
      return;
    }

    try {
      auto collectionProxy =
          std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(m_bus.connection(), kSecretServiceBusName, collection));
      m_pendingCall = collectionProxy->callMethodAsync("Get")
                          .onInterface(kPropertiesInterface)
                          .withTimeout(kProbeTimeout)
                          .withArguments(std::string{kSecretCollectionInterface}, std::string{"Locked"})
                          .uponReplyInvoke([this, generation, collectionProxy](
                                               std::optional<sdbus::Error> propertyError, sdbus::Variant lockedValue
                                           ) {
                            if (!isCurrent(generation)) {
                              return;
                            }
                            if (propertyError.has_value()) {
                              kLog.debug("secret default collection lock probe failed: {}", propertyError->what());
                              finish(generation, false);
                              return;
                            }
                            try {
                              finish(generation, !lockedValue.get<bool>());
                            } catch (const sdbus::Error& e) {
                              kLog.debug("secret default collection lock value invalid: {}", e.what());
                              finish(generation, false);
                            }
                          });
    } catch (const sdbus::Error& e) {
      kLog.debug("secret default collection lock probe dispatch failed: {}", e.what());
      finish(generation, false);
    }
  }

  void finish(std::uint64_t generation, bool unlocked) {
    if (!isCurrent(generation)) {
      return;
    }

    m_pendingCall = {};
    m_inFlight = false;
    const bool rerun = std::exchange(m_rerunRequested, false);
    if (m_callback) {
      m_callback(unlocked);
    }
    if (rerun && generation == m_generation) {
      request();
    }
  }

  SessionBus& m_bus;
  ResultCallback m_callback;
  std::unique_ptr<sdbus::IProxy> m_serviceProxy;
  sdbus::PendingAsyncCall m_pendingCall;
  std::uint64_t m_generation = 0;
  bool m_inFlight = false;
  bool m_rerunRequested = false;
};

SecretCollectionProbe::SecretCollectionProbe(SessionBus& bus, ResultCallback callback)
    : m_impl(std::make_unique<Impl>(bus, std::move(callback))) {}

SecretCollectionProbe::~SecretCollectionProbe() = default;

void SecretCollectionProbe::request() { m_impl->request(); }

void SecretCollectionProbe::invalidate() { m_impl->invalidate(); }
