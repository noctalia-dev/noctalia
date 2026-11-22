#include "dbus/bluetooth/bluetooth_uuid_names.h"
#include "tests/test_check.h"

#include <cstdint>
#include <optional>

int main() {
  // The 128-bit forms BlueZ actually passes to AuthorizeService.
  TEST_CHECK(bluetoothServiceLabel("0000110b-0000-1000-8000-00805f9b34fb") == "Audio Sink");
  TEST_CHECK(bluetoothServiceLabel("0000110a-0000-1000-8000-00805f9b34fb") == "Audio Source");
  TEST_CHECK(bluetoothServiceLabel("0000111e-0000-1000-8000-00805f9b34fb") == "Hands-Free");
  TEST_CHECK(bluetoothServiceLabel("00001124-0000-1000-8000-00805f9b34fb") == "Human Interface Device");
  TEST_CHECK(bluetoothServiceLabel("00001105-0000-1000-8000-00805f9b34fb") == "OBEX Object Push");
  TEST_CHECK(bluetoothServiceLabel("0000180f-0000-1000-8000-00805f9b34fb") == "Battery");
  TEST_CHECK(bluetoothServiceLabel("00001855-0000-1000-8000-00805f9b34fb") == "Telephony and Media Audio");
  TEST_CHECK(bluetoothServiceLabel("00001860-0000-1000-8000-00805f9b34fb") == "Tire Pressure Monitoring System");
  TEST_CHECK(bluetoothServiceLabel("00000001-0000-1000-8000-00805F9B34FB") == "SDP");
  TEST_CHECK(bluetoothServiceLabel("00000007-0000-1000-8000-00805f9b34fb") == "ATT");
  // ESDP UPnP L2CAP has no IP segment.
  TEST_CHECK(bluetoothServiceLabel("00001302-0000-1000-8000-00805f9b34fb") == "UPnP L2CAP");

  // Short forms, as they appear in SDP records and config files.
  TEST_CHECK(bluetoothServiceLabel("110b") == "Audio Sink");

  // Assigned-number gaps fall back to the readable short form rather than the full UUID.
  TEST_CHECK(bluetoothServiceLabel("00001fff-0000-1000-8000-00805f9b34fb") == "0x1fff");

  // Vendor UUIDs: named where we know them, echoed lowercase otherwise.
  TEST_CHECK(bluetoothServiceLabel("7905F431-B5CE-4E99-A40F-4B1E122D00D0") == "Apple Notification Center");
  TEST_CHECK(bluetoothServiceLabel("12345678-1234-5678-1234-56789abcdef0") == "12345678-1234-5678-1234-56789abcdef0");

  TEST_CHECK(bluetoothServiceLabel("").empty());
  TEST_CHECK(bluetoothServiceLabel("not-a-uuid") == "not-a-uuid");

  TEST_CHECK(bluetoothUuidShortId("0000110b-0000-1000-8000-00805f9b34fb") == std::optional<std::uint16_t>{0x110b});
  TEST_CHECK(bluetoothUuidShortId("110b") == std::optional<std::uint16_t>{0x110b});
  // A 32-bit value that does not fit 16 bits is not a short alias.
  TEST_CHECK(!bluetoothUuidShortId("1234110b-0000-1000-8000-00805f9b34fb").has_value());
  TEST_CHECK(!bluetoothUuidShortId("12345678-1234-5678-1234-56789abcdef0").has_value());
  TEST_CHECK(!bluetoothUuidShortId("0000110g-0000-1000-8000-00805f9b34fb").has_value());

  return 0;
}
