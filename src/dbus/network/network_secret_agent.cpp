#include "dbus/network/network_secret_agent.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IObject.h>
#include <sdbus-c++/MethodResult.h>
#include <sdbus-c++/Types.h>
#include <sdbus-c++/VTableItems.h>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("network");

  using SecretsDict = std::map<std::string, std::map<std::string, sdbus::Variant>>;

  // No well-known name on the system bus — NM tracks the agent by the sender's
  // unique name from Register, and claiming a name would need a dbus policy.
  const sdbus::ObjectPath kAgentObjectPath{"/org/freedesktop/NetworkManager/SecretAgent"};
  constexpr auto kAgentInterface = "org.freedesktop.NetworkManager.SecretAgent";
  constexpr auto kAgentIdentifier = "org.noctalia.SecretAgent";

  const sdbus::ServiceName kNmBusName{"org.freedesktop.NetworkManager"};
  const sdbus::ObjectPath kAgentManagerObjectPath{"/org/freedesktop/NetworkManager/AgentManager"};
  constexpr auto kAgentManagerInterface = "org.freedesktop.NetworkManager.AgentManager";

  constexpr std::uint32_t kNmSecretAgentGetSecretsFlagAllowInteraction = 0x1;

  constexpr auto kWirelessSettingName = "802-11-wireless";
  constexpr auto kWirelessSecuritySettingName = "802-11-wireless-security";
  constexpr auto kPskKey = "psk";

  std::string extractSsid(const SecretsDict& connection) {
    auto wifiIt = connection.find(kWirelessSettingName);
    if (wifiIt == connection.end()) {
      return {};
    }
    auto ssidIt = wifiIt->second.find("ssid");
    if (ssidIt == wifiIt->second.end()) {
      return {};
    }
    try {
      const auto bytes = ssidIt->second.get<std::vector<std::uint8_t>>();
      return std::string(bytes.begin(), bytes.end());
    } catch (const sdbus::Error&) {
      return {};
    }
  }

} // namespace

struct NetworkSecretAgent::Impl {
  SystemBus& bus;
  std::unique_ptr<sdbus::IObject> object;
  RequestCallback requestCallback;
  std::optional<sdbus::Result<SecretsDict>> pendingResult;
  std::string pendingSettingName;

  explicit Impl(SystemBus& b) : bus(b) {}

  void onGetSecrets(
      sdbus::Result<SecretsDict>&& result, SecretsDict connection, sdbus::ObjectPath connectionPath,
      std::string settingName, std::vector<std::string> /*hints*/, std::uint32_t flags
  ) {
    if ((flags & kNmSecretAgentGetSecretsFlagAllowInteraction) == 0U) {
      kLog.debug("GetSecrets without ALLOW_INTERACTION -> NoSecrets");
      result.returnError(
          sdbus::Error{
              sdbus::Error::Name{"org.freedesktop.NetworkManager.SecretManager.NoSecrets"},
              "no interactive prompt permitted"
          }
      );
      return;
    }
    if (settingName != kWirelessSecuritySettingName) {
      kLog.debug("GetSecrets for unsupported setting \"{}\" -> NoSecrets", settingName);
      result.returnError(
          sdbus::Error{
              sdbus::Error::Name{"org.freedesktop.NetworkManager.SecretManager.NoSecrets"}, "unsupported setting"
          }
      );
      return;
    }
    if (pendingResult.has_value()) {
      kLog.debug("GetSecrets while another is pending -> NoSecrets");
      result.returnError(
          sdbus::Error{
              sdbus::Error::Name{"org.freedesktop.NetworkManager.SecretManager.NoSecrets"},
              "another secret request is already pending"
          }
      );
      return;
    }

    SecretRequest request;
    request.ssid = extractSsid(connection);
    request.settingName = settingName;
    kLog.info("GetSecrets prompt ssid=\"{}\" path={}", request.ssid, std::string(connectionPath));

    pendingResult = std::move(result);
    pendingSettingName = settingName;

    if (requestCallback) {
      requestCallback(request);
    } else {
      // No UI available; fail fast so NM doesn't sit waiting.
      cancelPending("no UI handler");
    }
  }

  void cancelPending(const std::string& reason) {
    if (!pendingResult.has_value()) {
      return;
    }
    pendingResult->returnError(
        sdbus::Error{sdbus::Error::Name{"org.freedesktop.NetworkManager.SecretManager.UserCanceled"}, reason}
    );
    pendingResult.reset();
    pendingSettingName.clear();
  }

  void submitPending(const std::string& psk) {
    if (!pendingResult.has_value()) {
      return;
    }
    SecretsDict secrets;
    secrets[pendingSettingName][kPskKey] = sdbus::Variant{psk};
    pendingResult->returnResults(secrets);
    pendingResult.reset();
    pendingSettingName.clear();
  }
};

NetworkSecretAgent::NetworkSecretAgent(SystemBus& bus) : m_impl(std::make_unique<Impl>(bus)) {
  m_impl->object = sdbus::createObject(bus.connection(), kAgentObjectPath);

  m_impl->object
      ->addVTable(
          sdbus::registerMethod("GetSecrets")
              .withInputParamNames("connection", "connection_path", "setting_name", "hints", "flags")
              .withOutputParamNames("secrets")
              .implementedAs([this](
                                 sdbus::Result<SecretsDict>&& result, SecretsDict connection,
                                 sdbus::ObjectPath connectionPath, std::string settingName,
                                 std::vector<std::string> hints, std::uint32_t flags
                             ) {
                m_impl->onGetSecrets(
                    std::move(result), std::move(connection), std::move(connectionPath), std::move(settingName),
                    std::move(hints), flags
                );
              }),
          sdbus::registerMethod("CancelGetSecrets")
              .withInputParamNames("connection_path", "setting_name")
              .implementedAs([this](const sdbus::ObjectPath& /*connectionPath*/, const std::string& /*settingName*/) {
                m_impl->cancelPending("NM canceled");
              }),
          sdbus::registerMethod("SaveSecrets")
              .withInputParamNames("connection", "connection_path")
              .implementedAs([](const SecretsDict& /*connection*/, const sdbus::ObjectPath& /*connectionPath*/) {}),
          sdbus::registerMethod("DeleteSecrets")
              .withInputParamNames("connection", "connection_path")
              .implementedAs([](const SecretsDict& /*connection*/, const sdbus::ObjectPath& /*connectionPath*/) {})
      )
      .forInterface(kAgentInterface);

  // Register with NM's agent manager.
  try {
    auto agentManager = sdbus::createProxy(bus.connection(), kNmBusName, kAgentManagerObjectPath);
    agentManager->callMethod("Register")
        .onInterface(kAgentManagerInterface)
        .withArguments(std::string(kAgentIdentifier));
    kLog.info("registered NetworkManager secret agent as {}", kAgentIdentifier);
  } catch (const sdbus::Error& e) {
    kLog.warn("secret agent registration failed: {}", e.what());
  }
}

NetworkSecretAgent::~NetworkSecretAgent() {
  if (m_impl == nullptr) {
    return;
  }
  m_impl->cancelPending("agent shutting down");
  try {
    auto agentManager = sdbus::createProxy(m_impl->bus.connection(), kNmBusName, kAgentManagerObjectPath);
    agentManager->callMethod("Unregister").onInterface(kAgentManagerInterface);
  } catch (const sdbus::Error& e) {
    kLog.debug("secret agent unregister failed: {}", e.what());
  }
}

void NetworkSecretAgent::setRequestCallback(RequestCallback callback) {
  if (m_impl != nullptr) {
    m_impl->requestCallback = std::move(callback);
  }
}

void NetworkSecretAgent::submitSecret(const std::string& psk) {
  if (m_impl != nullptr) {
    m_impl->submitPending(psk);
  }
}

void NetworkSecretAgent::cancelSecret() {
  if (m_impl != nullptr) {
    m_impl->cancelPending("user canceled");
  }
}

bool NetworkSecretAgent::hasPendingRequest() const noexcept {
  return m_impl != nullptr && m_impl->pendingResult.has_value();
}
