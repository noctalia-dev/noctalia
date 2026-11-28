#pragma once

#include "security/security_hardware_parsers.h"

#include <functional>
#include <string>

class Flex;
class Input;
class SecurityKeyManager;

namespace settings {

  // Transient UI state of the Security page blocks that must survive content rebuilds.
  struct SecurityHardwareUiState {
    // Destructive buttons ask for a second press: what each one armed.
    std::string pendingCredentialRemove; // key handle
    bool pendingGpgRemove = false;
    std::string pendingSshRemove;  // name in ~/.ssh
    std::string pendingSshDelete;  // name in ~/.ssh
    std::string sshPersistOpenFor; // the key whose connection reuse is being set
  };

  // The new resident SSH key sheet's fields.
  struct SshCreateForm {
    security_hw::SshCreateOptions options;
    std::string problem; // a translation key, or ""
  };

  struct SecurityHardwareActions {
    std::function<void()> requestRebuild;
    std::function<void()> browseGpgPublicKey;
    std::function<void()> openSshCreateSheet;
    // Copies text to the clipboard; returns whether it did.
    std::function<bool(const std::string&)> copyText;
    bool offline = false;
  };

  void buildFido2Block(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  );
  void buildGpgCardBlock(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  );
  void buildSshBlock(
      Flex& block, SecurityKeyManager& manager, SecurityHardwareUiState& state, const SecurityHardwareActions& actions,
      float scale
  );

  // The key's PIN for the operation waiting on it, then the touch while it runs.
  // <pinInput> receives the PIN field (null while the touch is awaited), to focus it.
  void buildSecurityKeyPinSheet(
      Flex& body, SecurityKeyManager& manager, std::string& pin, Input** pinInput, const std::function<void()>& close,
      float scale
  );
  // A new resident SSH key: type, application, user ID, comment, PIN and touch for every use.
  void buildSshCreateSheet(
      Flex& body, SshCreateForm& form, const std::function<void()>& submit, const std::function<void()>& rebuild,
      const std::function<void()>& close, float scale
  );

} // namespace settings
