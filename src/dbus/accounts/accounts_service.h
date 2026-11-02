#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>

class SystemBus;

namespace sdbus {
  class IProxy;
}

class AccountsService {
public:
  using ChangeCallback = std::function<void()>;

  explicit AccountsService(SystemBus& bus);

  void setChangeCallback(ChangeCallback callback);

  uid_t sessionUid() const noexcept { return m_sessionUid; }
  const std::string& iconFile() const noexcept { return m_iconFile; }

  [[nodiscard]] bool setIconFile(std::string_view filename);

private:
  void readIconFile();
  void emitChangedIfNeeded(std::string_view nextIconFile);

  SystemBus& m_bus;
  std::unique_ptr<sdbus::IProxy> m_accountsProxy;
  std::unique_ptr<sdbus::IProxy> m_userProxy;
  uid_t m_sessionUid = 0;
  std::string m_iconFile;
  ChangeCallback m_changeCallback;
};
