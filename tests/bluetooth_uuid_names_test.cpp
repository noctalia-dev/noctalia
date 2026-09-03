#include "dbus/bluetooth/bluetooth_uuid_names.h"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

namespace {

  bool expectLabel(std::string_view uuid, std::string_view expected, const char* message) {
    const std::string actual = bluetoothServiceLabel(uuid);
    if (actual != expected) {
      std::println(stderr, "bluetooth_uuid_names_test: {}: expected '{}', got '{}'", message, expected, actual);
      return false;
    }
    return true;
  }

  bool expectShortId(std::string_view uuid, bool hasValue, std::uint16_t expected, const char* message) {
    const auto actual = bluetoothUuidShortId(uuid);
    if (actual.has_value() != hasValue || (hasValue && *actual != expected)) {
      std::println(stderr, "bluetooth_uuid_names_test: {}: unexpected short id for '{}'", message, uuid);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  // The 128-bit forms BlueZ actually passes to AuthorizeService.
  ok = expectLabel("0000110b-0000-1000-8000-00805f9b34fb", "Audio Sink", "A2DP sink") && ok;
  ok = expectLabel("0000110a-0000-1000-8000-00805f9b34fb", "Audio Source", "A2DP source") && ok;
  ok = expectLabel("0000111e-0000-1000-8000-00805f9b34fb", "Hands-Free", "HFP") && ok;
  ok = expectLabel("00001124-0000-1000-8000-00805f9b34fb", "Human Interface Device", "HID") && ok;
  ok = expectLabel("00001105-0000-1000-8000-00805f9b34fb", "OBEX Object Push", "OPP") && ok;
  ok = expectLabel("0000180f-0000-1000-8000-00805f9b34fb", "Battery", "GATT battery service") && ok;
  ok = expectLabel("00001855-0000-1000-8000-00805f9b34fb", "Telephony and Media Audio", "mid-table entry") && ok;
  ok = expectLabel("00001860-0000-1000-8000-00805f9b34fb", "Tire Pressure Monitoring System", "last table entry") && ok;
  ok = expectLabel("00000001-0000-1000-8000-00805F9B34FB", "SDP", "uppercase input") && ok;
  ok = expectLabel("00000007-0000-1000-8000-00805f9b34fb", "ATT", "attribute protocol id") && ok;
  ok = expectLabel("00001302-0000-1000-8000-00805f9b34fb", "UPnP L2CAP", "ESDP UPnP L2CAP has no IP segment") && ok;

  // Short forms, as they appear in SDP records and config files.
  ok = expectLabel("110b", "Audio Sink", "16-bit short form") && ok;

  // Assigned-number gaps fall back to the readable short form rather than the full UUID.
  ok = expectLabel("00001fff-0000-1000-8000-00805f9b34fb", "0x1fff", "unassigned base UUID") && ok;

  // Vendor UUIDs: named where we know them, echoed lowercase otherwise.
  ok = expectLabel("7905F431-B5CE-4E99-A40F-4B1E122D00D0", "Apple Notification Center", "ANCS") && ok;
  ok =
      expectLabel("12345678-1234-5678-1234-56789abcdef0", "12345678-1234-5678-1234-56789abcdef0", "unknown vendor UUID")
      && ok;

  ok = expectLabel("", "", "empty uuid") && ok;
  ok = expectLabel("not-a-uuid", "not-a-uuid", "malformed uuid") && ok;

  ok = expectShortId("0000110b-0000-1000-8000-00805f9b34fb", true, 0x110b, "base UUID") && ok;
  ok = expectShortId("110b", true, 0x110b, "short form") && ok;
  // A 32-bit value that does not fit 16 bits is not a short alias.
  ok = expectShortId("1234110b-0000-1000-8000-00805f9b34fb", false, 0, "32-bit base UUID") && ok;
  ok = expectShortId("12345678-1234-5678-1234-56789abcdef0", false, 0, "vendor UUID") && ok;
  ok = expectShortId("0000110g-0000-1000-8000-00805f9b34fb", false, 0, "non-hex digits") && ok;

  return ok ? 0 : 1;
}
