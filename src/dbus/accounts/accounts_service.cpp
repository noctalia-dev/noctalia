#include "dbus/accounts/accounts_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <cstdint>
#include <optional>
#include <sdbus-c++/Error.h>
#include <sdbus-c++/IProxy.h>
#include <sdbus-c++/Types.h>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  constexpr Logger kLog("accounts");

  const sdbus::ServiceName kAccountsBusName{"org.freedesktop.Accounts"};
  const sdbus::ObjectPath kAccountsObjectPath{"/org/freedesktop/Accounts"};
  constexpr auto kAccountsInterface = "org.freedesktop.Accounts";
  constexpr auto kUserInterface = "org.freedesktop.Accounts.User";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";

  template <typename T>
  T getPropertyOr(sdbus::IProxy& proxy, std::string_view iface, std::string_view propertyName, T fallback) {
    try {
      const sdbus::Variant value = proxy.getProperty(propertyName).onInterface(iface);
      return value.get<T>();
    } catch (const sdbus::Error&) {
      return fallback;
    }
  }

  [[nodiscard]] std::optional<uid_t> readUserUid(sdbus::IProxy& userProxy) {
    try {
      const sdbus::Variant value = userProxy.getProperty("Uid").onInterface(kUserInterface);
      const auto uid = value.get<std::uint64_t>();
      return static_cast<uid_t>(uid);
    } catch (const sdbus::Error& e) {
      kLog.warn("failed to read Accounts.User Uid: {}", e.what());
      return std::nullopt;
    }
  }

} // namespace

AccountsService::AccountsService(SystemBus& bus) : m_bus(bus) {
  m_sessionUid = ::getuid();
  if (m_sessionUid == static_cast<uid_t>(-1)) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "failed to resolve session uid via getuid()"
    );
  }

  m_accountsProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, kAccountsObjectPath);

  sdbus::ObjectPath userPath;
  m_accountsProxy->callMethod("FindUserById")
      .onInterface(kAccountsInterface)
      .withArguments(static_cast<std::int64_t>(m_sessionUid))
      .storeResultsTo(userPath);

  m_userProxy = sdbus::createProxy(m_bus.connection(), kAccountsBusName, userPath);

  const std::optional<uid_t> reportedUid = readUserUid(*m_userProxy);
  if (!reportedUid.has_value() || *reportedUid != m_sessionUid) {
    throw sdbus::Error(
        sdbus::Error::Name{"org.freedesktop.DBus.Error.Failed"}, "AccountsService user Uid does not match session uid"
    );
  }

  m_userProxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](
                const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                const std::vector<std::string>& /*invalidatedProperties*/
            ) {
        if (interfaceName != kUserInterface) {
          return;
        }
        const auto it = changedProperties.find("IconFile");
        if (it == changedProperties.end()) {
          return;
        }
        try {
          emitChangedIfNeeded(it->second.get<std::string>());
        } catch (const sdbus::Error& e) {
          kLog.warn("failed to decode IconFile property change: {}", e.what());
        }
      });

  readIconFile();
  kLog.info("session user avatar path: {}", m_iconFile.empty() ? "<unset>" : m_iconFile);
}

void AccountsService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

bool AccountsService::setIconFile(std::string_view filename) {
  if (filename.empty()) {
    kLog.warn("SetIconFile rejected: filename is empty");
    return false;
  }
  if (m_userProxy == nullptr) {
    kLog.warn("SetIconFile rejected: user proxy is unavailable");
    return false;
  }

  try {
    m_userProxy->callMethod("SetIconFile").onInterface(kUserInterface).withArguments(std::string(filename));
    const std::string previousIconFile = m_iconFile;
    readIconFile();
    if (m_iconFile == previousIconFile && m_changeCallback) {
      m_changeCallback();
    }
    return true;
  } catch (const sdbus::Error& e) {
    kLog.warn("SetIconFile failed: {}", e.what());
    return false;
  }
}

void AccountsService::readIconFile() {
  if (m_userProxy == nullptr) {
    return;
  }
  const auto iconFile = getPropertyOr<std::string>(*m_userProxy, kUserInterface, "IconFile", std::string{});
  emitChangedIfNeeded(iconFile);
}

void AccountsService::emitChangedIfNeeded(std::string_view nextIconFile) {
  const std::string next(nextIconFile);
  if (next == m_iconFile) {
    return;
  }
  m_iconFile = next;
  if (m_changeCallback) {
    m_changeCallback();
  }
}
