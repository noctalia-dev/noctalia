#include "dbus/network/network_secret_agent.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "dbus/network/vpn_auth_dialog.h"
#include "dbus/system_bus.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/IObject.h>
#include <sdbus-c++/IProxy.h>
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
  const sdbus::ServiceName kDbusBusName{"org.freedesktop.DBus"};
  const sdbus::ObjectPath kDbusObjectPath{"/org/freedesktop/DBus"};
  constexpr auto kDbusInterface = "org.freedesktop.DBus";

  constexpr std::uint32_t kNmSecretAgentGetSecretsFlagAllowInteraction = 0x1;
  // NM_SECRET_AGENT_CAPABILITY_VPN_HINTS: we pass GetSecrets hints on to the VPN
  // auth dialog. NM only starts a VPN with ConnectInteractive, which lets the plugin
  // ask for more secrets mid-connection (e.g. an OTP after the server's challenge),
  // when every agent of the user declares this; otherwise the challenge just fails.
  constexpr std::uint32_t kNmSecretAgentCapabilityVpnHints = 0x1;

  constexpr auto kWirelessSettingName = "802-11-wireless";
  constexpr auto kWirelessSecuritySettingName = "802-11-wireless-security";
  constexpr auto kEnterpriseSettingName = "802-1x";
  constexpr auto kVpnSettingName = "vpn";
  constexpr auto kPskKey = "psk";
  constexpr auto kEapPasswordKey = "password";

  // Generous for a key file describing a handful of fields; a helper printing more
  // than this is not speaking the protocol.
  constexpr std::size_t kMaxAuthDialogOutput = 64 * 1024;

  constexpr auto kNoSecretsError = "org.freedesktop.NetworkManager.SecretManager.NoSecrets";
  constexpr auto kUserCanceledError = "org.freedesktop.NetworkManager.SecretManager.UserCanceled";

  // Reads one property of one setting from a GetSecrets connection dict.
  template <typename T>
  std::optional<T> settingValue(const SecretsDict& connection, const std::string& setting, const std::string& key) {
    const auto settingIt = connection.find(setting);
    if (settingIt == connection.end()) {
      return std::nullopt;
    }
    const auto valueIt = settingIt->second.find(key);
    if (valueIt == settingIt->second.end()) {
      return std::nullopt;
    }
    try {
      return valueIt->second.get<T>();
    } catch (const sdbus::Error&) {
      return std::nullopt;
    }
  }

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
  std::unique_ptr<sdbus::IProxy> nameWatcher;
  bool registered = false;
  RequestCallback requestCallback;
  std::optional<sdbus::Result<SecretsDict>> pendingResult;
  std::string pendingSettingName;
  // VPN requests: the running auth dialog (set while it runs), which run its exit
  // belongs to, and the secrets it already knew, sent along with the user's answers.
  std::shared_ptr<std::atomic<bool>> authDialogCancel;
  std::uint64_t authDialogGeneration = 0;
  vpn_auth::StringMap pendingVpnSecrets;
  // Expires with the agent, so a dialog finishing after teardown is ignored.
  std::shared_ptr<int> lifetime = std::make_shared<int>(0);

  explicit Impl(SystemBus& b) : bus(b) {}

  void registerAgent() {
    try {
      auto agentManager = sdbus::createProxy(bus.connection(), kNmBusName, kAgentManagerObjectPath);
      agentManager->callMethod("RegisterWithCapabilities")
          .onInterface(kAgentManagerInterface)
          .withArguments(std::string(kAgentIdentifier), kNmSecretAgentCapabilityVpnHints);
      registered = true;
      kLog.info("registered NetworkManager secret agent as {}", kAgentIdentifier);
    } catch (const sdbus::Error& e) {
      registered = false;
      kLog.warn("secret agent registration failed: {}", e.what());
    }
  }

  void onGetSecrets(
      sdbus::Result<SecretsDict>&& result, const SecretsDict& connection, sdbus::ObjectPath connectionPath,
      std::string settingName, const std::vector<std::string>& hints, std::uint32_t flags
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
    // NM asks for the pre-shared key under "802-11-wireless-security" and for EAP
    // credentials under "802-1x". Rejecting the latter is what leaves an
    // enterprise profile unable to re-authenticate after its password changes.
    if (settingName != kWirelessSecuritySettingName
        && settingName != kEnterpriseSettingName
        && settingName != kVpnSettingName) {
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

    if (settingName == kVpnSettingName) {
      startAuthDialog(std::move(result), connection, hints, flags);
      return;
    }

    SecretRequest request;
    request.ssid = extractSsid(connection);
    request.settingName = settingName;
    kLog.info("GetSecrets prompt ssid=\"{}\" path={}", request.ssid, std::string(std::move(connectionPath)));

    pendingResult = std::move(result);
    pendingSettingName = settingName;

    if (requestCallback) {
      requestCallback(request);
    } else {
      // No UI available; fail fast so NM doesn't sit waiting.
      cancelPending("no UI handler");
    }
  }

  void startAuthDialog(
      sdbus::Result<SecretsDict>&& result, const SecretsDict& connection, std::vector<std::string> hints,
      std::uint32_t flags
  ) {
    vpn_auth::DialogRequest request{
        .connectionUuid = settingValue<std::string>(connection, "connection", "uuid").value_or(""),
        .connectionId = settingValue<std::string>(connection, "connection", "id").value_or(""),
        .serviceType = settingValue<std::string>(connection, kVpnSettingName, "service-type").value_or(""),
        .hints = std::move(hints),
        .flags = flags,
        .data = settingValue<vpn_auth::StringMap>(connection, kVpnSettingName, "data").value_or(vpn_auth::StringMap{}),
        .secrets =
            settingValue<vpn_auth::StringMap>(connection, kVpnSettingName, "secrets").value_or(vpn_auth::StringMap{}),
    };
    const auto plugin = vpn_auth::findPlugin(request.serviceType, vpn_auth::defaultPluginDirs());
    if (!plugin.has_value() || plugin->authDialog.empty()) {
      kLog.warn("GetSecrets vpn=\"{}\": no auth dialog for {}", request.connectionId, request.serviceType);
      result.returnError(sdbus::Error{sdbus::Error::Name{kNoSecretsError}, "no auth dialog for VPN service"});
      return;
    }
    kLog.info(
        "GetSecrets vpn=\"{}\" via {}{}", request.connectionId, plugin->authDialog,
        plugin->externalUiMode ? " (external UI)" : ""
    );

    pendingResult = std::move(result);
    pendingSettingName = kVpnSettingName;
    const std::uint64_t generation = ++authDialogGeneration;
    authDialogCancel = std::make_shared<std::atomic<bool>>(false);

    process::RunOptions options{
        .maxOutputBytes = kMaxAuthDialogOutput,
        .cancel = authDialogCancel,
        .stdinData = vpn_auth::dialogInput(request),
    };
    const std::weak_ptr<int> alive = lifetime;
    const bool started = process::runAsync(
        vpn_auth::dialogArgs(*plugin, request),
        process::RunCallbacks{
            .onExit =
                [this, alive, generation, externalUi = plugin->externalUiMode,
                 name = request.connectionId](process::RunResult run) {
                  DeferredCall::callLater([this, alive, generation, externalUi, name, run = std::move(run)]() mutable {
                    if (!alive.expired()) {
                      onAuthDialogExit(generation, externalUi, name, std::move(run));
                    }
                  });
                },
        },
        std::move(options)
    );
    if (!started) {
      kLog.warn("GetSecrets vpn=\"{}\": failed to start {}", request.connectionId, plugin->authDialog);
      rejectPending(kNoSecretsError, "auth dialog failed to start");
    }
  }

  void onAuthDialogExit(std::uint64_t generation, bool externalUi, const std::string& name, process::RunResult run) {
    // A cancel or a newer request already answered the request this run belonged to.
    if (generation != authDialogGeneration || !pendingResult.has_value() || !authDialogCancel) {
      return;
    }
    authDialogCancel.reset();
    if (run.exitCode != 0 || run.timedOut) {
      // Helpers exit non-zero when the user dismisses their own window.
      kLog.info("auth dialog for vpn=\"{}\" exited with {}: {}", name, run.exitCode, run.err);
      cancelPending("auth dialog canceled");
      return;
    }
    if (!externalUi) {
      replyVpn(vpn_auth::parseLegacyOutput(run.out));
      return;
    }

    auto prompt = vpn_auth::parseExternalUiOutput(run.out);
    if (!prompt.has_value()) {
      kLog.warn("auth dialog for vpn=\"{}\" printed an unreadable prompt: {}", name, prompt.error());
      rejectPending(kNoSecretsError, "unreadable auth dialog output");
      return;
    }
    pendingVpnSecrets = std::move(prompt->known);
    if (prompt->fields.empty()) {
      replyVpn({});
      return;
    }

    SecretRequest request;
    request.settingName = kVpnSettingName;
    request.connectionName = name;
    request.message = std::move(prompt->message);
    for (auto& field : prompt->fields) {
      request.fields.push_back({
          .key = std::move(field.key),
          .label = std::move(field.label),
          .value = std::move(field.value),
          .secret = field.secret,
          .echo = field.echo,
      });
    }
    if (requestCallback) {
      requestCallback(request);
    } else {
      cancelPending("no UI handler");
    }
  }

  void rejectPending(const char* errorName, const std::string& reason) {
    if (authDialogCancel) {
      authDialogCancel->store(true);
      authDialogCancel.reset();
    }
    pendingVpnSecrets.clear();
    if (!pendingResult.has_value()) {
      return;
    }
    pendingResult->returnError(sdbus::Error{sdbus::Error::Name{errorName}, reason});
    pendingResult.reset();
    pendingSettingName.clear();
  }

  void cancelPending(const std::string& reason) { rejectPending(kUserCanceledError, reason); }

  void replyVpn(const vpn_auth::StringMap& values) {
    if (!pendingResult.has_value() || pendingSettingName != kVpnSettingName) {
      return;
    }
    vpn_auth::StringMap merged = std::move(pendingVpnSecrets);
    pendingVpnSecrets.clear();
    for (const auto& [key, value] : values) {
      merged.insert_or_assign(key, value);
    }
    SecretsDict secrets;
    secrets[kVpnSettingName]["secrets"] = sdbus::Variant{merged};
    pendingResult->returnResults(secrets);
    pendingResult.reset();
    pendingSettingName.clear();
  }

  void submitPending(const std::string& secret) {
    if (!pendingResult.has_value() || pendingSettingName == kVpnSettingName) {
      return;
    }
    SecretsDict secrets;
    const auto* key = pendingSettingName == kEnterpriseSettingName ? kEapPasswordKey : kPskKey;
    secrets[pendingSettingName][key] = sdbus::Variant{secret};
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
                                 sdbus::Result<SecretsDict>&& result, const SecretsDict& connection,
                                 sdbus::ObjectPath connectionPath, std::string settingName,
                                 const std::vector<std::string>& hints, std::uint32_t flags
                             ) {
                m_impl->onGetSecrets(
                    std::move(result), connection, std::move(connectionPath), std::move(settingName), hints, flags
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

  m_impl->nameWatcher = sdbus::createProxy(bus.connection(), kDbusBusName, kDbusObjectPath);
  m_impl->nameWatcher->uponSignal("NameOwnerChanged")
      .onInterface(kDbusInterface)
      .call([this](const std::string& name, const std::string& oldOwner, const std::string& newOwner) {
        if (name != kNmBusName || (oldOwner.empty() && !newOwner.empty() && m_impl->registered)) {
          return;
        }
        m_impl->registered = false;
        m_impl->cancelPending("NetworkManager owner changed");
        if (!newOwner.empty()) {
          m_impl->registerAgent();
        }
      });
  if (bus.nameHasOwner(kNmBusName)) {
    m_impl->registerAgent();
  }
}

NetworkSecretAgent::~NetworkSecretAgent() {
  if (m_impl == nullptr) {
    return;
  }
  m_impl->nameWatcher.reset();
  m_impl->cancelPending("agent shutting down");
  if (!m_impl->registered) {
    return;
  }
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

void NetworkSecretAgent::submitSecret(const std::string& secret) {
  if (m_impl != nullptr) {
    m_impl->submitPending(secret);
  }
}

void NetworkSecretAgent::submitSecrets(const std::map<std::string, std::string>& values) {
  // Only once the auth dialog has run: before that, the UI has nothing to answer.
  if (m_impl != nullptr && !m_impl->authDialogCancel) {
    m_impl->replyVpn(values);
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
