#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// BlueZ hands the agent a raw 128-bit UUID in AuthorizeService ("Allow access to
// 0000110b-0000-1000-8000-00805f9b34fb?"), which tells the user nothing. These helpers
// turn it into the Bluetooth SIG service name where one is assigned.

// Short 16-bit alias of a UUID built on the Bluetooth base UUID
// (0000xxxx-0000-1000-8000-00805f9b34fb), or nullopt for a vendor UUID.
[[nodiscard]] std::optional<std::uint16_t> bluetoothUuidShortId(std::string_view uuid);

// SIG-assigned name for a UUID, or nullopt when the UUID is unassigned or vendor-specific.
[[nodiscard]] std::optional<std::string_view> bluetoothServiceName(std::string_view uuid);

// Always returns something a user can read: the SIG service name when known, else the
// short "0x110b" form for base UUIDs, else the UUID itself.
[[nodiscard]] std::string bluetoothServiceLabel(std::string_view uuid);
