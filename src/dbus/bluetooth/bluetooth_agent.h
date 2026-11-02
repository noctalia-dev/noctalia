#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SystemBus;

enum class BluetoothPairingKind : std::uint8_t {
  None,
  PinCode,          // Agent must return a string PIN entered by the user.
  Passkey,          // Agent must return a numeric passkey entered by the user.
  DisplayPinCode,   // Informational: display the PIN to the user.
  DisplayPasskey,   // Informational: display the passkey (with entered count).
  Confirm,          // Yes/no confirmation of a 6-digit code.
  Authorize,        // Yes/no for an incoming pairing.
  AuthorizeService, // Yes/no for a service (with uuid).
};

struct BluetoothPairingRequest {
  BluetoothPairingKind kind = BluetoothPairingKind::None;
  std::string devicePath;
  std::string deviceAlias; // filled in by the service/UI if known; agent leaves empty
  std::uint32_t passkey = 0;
  std::uint16_t entered = 0;
  std::string pin;
  std::string uuid;
};

// Registers an org.bluez.Agent1 on the system bus and forwards pairing
// interactions to the UI through a single-slot callback.
//
// Only one request is in flight at a time. Additional requests while another
// is pending are rejected with org.bluez.Error.Rejected so BlueZ falls back
// to a different agent / next handler.
class BluetoothAgent {
public:
  using RequestCallback = std::function<void(const BluetoothPairingRequest&)>;

  explicit BluetoothAgent(SystemBus& bus);
  ~BluetoothAgent();

  BluetoothAgent(const BluetoothAgent&) = delete;
  BluetoothAgent& operator=(const BluetoothAgent&) = delete;

  void setRequestCallback(RequestCallback callback);

  // Reply paths — safe no-ops if nothing is pending.
  void acceptConfirm(); // Confirm / Authorize / AuthorizeService / DisplayPinCode / DisplayPasskey
  void rejectConfirm();
  void submitPin(const std::string& pin);    // PinCode kind
  void submitPasskey(std::uint32_t passkey); // Passkey kind
  void cancelPending();

  bool hasPendingRequest() const noexcept;
  BluetoothPairingRequest pendingRequest() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
