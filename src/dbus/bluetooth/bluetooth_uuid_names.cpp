#include "dbus/bluetooth/bluetooth_uuid_names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <utility>

namespace {

  // Tail shared by every UUID derived from the Bluetooth base UUID.
  constexpr std::string_view kBaseSuffix = "-0000-1000-8000-00805f9b34fb";

  // Bluetooth SIG assigned numbers: SDP service classes (0x1000-0x14xx), GATT services
  // (0x18xx) and the protocol identifiers (0x00xx) that show up as SDP profile UUIDs.
  // Sorted by id so lookup can binary search.
  using ServiceName = std::pair<std::uint16_t, std::string_view>;

  // std::to_array keeps the size tied to the entries: a plain std::array<N> silently
  // pads with zero-id entries when N drifts, which breaks the sorted invariant below.
  constexpr auto kServiceNames = std::to_array<ServiceName>({
      {0x0001, "SDP"},
      {0x0003, "RFCOMM"},
      {0x0007, "ATT"},
      {0x0008, "OBEX"},
      {0x000f, "BNEP"},
      {0x0011, "HIDP"},
      {0x0017, "AVCTP"},
      {0x0019, "AVDTP"},
      {0x001f, "MCAP Data Channel"},
      {0x0100, "L2CAP"},
      {0x1000, "Service Discovery Server"},
      {0x1001, "Browse Group Descriptor"},
      {0x1002, "Public Browse Root"},
      {0x1101, "Serial Port"},
      {0x1102, "LAN Access Using PPP"},
      {0x1103, "Dial-up Networking"},
      {0x1104, "IrMC Sync"},
      {0x1105, "OBEX Object Push"},
      {0x1106, "OBEX File Transfer"},
      {0x1107, "IrMC Sync Command"},
      {0x1108, "Headset"},
      {0x1109, "Cordless Telephony"},
      {0x110a, "Audio Source"},
      {0x110b, "Audio Sink"},
      {0x110c, "A/V Remote Control Target"},
      {0x110d, "Advanced Audio Distribution"},
      {0x110e, "A/V Remote Control"},
      {0x110f, "A/V Remote Control Controller"},
      {0x1110, "Intercom"},
      {0x1111, "Fax"},
      {0x1112, "Headset Audio Gateway"},
      {0x1113, "WAP"},
      {0x1114, "WAP Client"},
      {0x1115, "Personal Area Networking User"},
      {0x1116, "Network Access Point"},
      {0x1117, "Group Ad-hoc Network"},
      {0x1118, "Direct Printing"},
      {0x1119, "Reference Printing"},
      {0x111a, "Basic Imaging"},
      {0x111b, "Imaging Responder"},
      {0x111c, "Imaging Automatic Archive"},
      {0x111d, "Imaging Referenced Objects"},
      {0x111e, "Hands-Free"},
      {0x111f, "Hands-Free Audio Gateway"},
      {0x1120, "Direct Printing Reference Objects"},
      {0x1121, "Reflected UI"},
      {0x1122, "Basic Printing"},
      {0x1123, "Printing Status"},
      {0x1124, "Human Interface Device"},
      {0x1125, "Hardcopy Cable Replacement"},
      {0x1126, "Hardcopy Print"},
      {0x1127, "Hardcopy Scan"},
      {0x1128, "Common ISDN Access"},
      {0x112d, "SIM Access"},
      {0x112e, "Phonebook Access Client"},
      {0x112f, "Phonebook Access Server"},
      {0x1130, "Phonebook Access"},
      {0x1131, "Headset Device"},
      {0x1132, "Message Access Server"},
      {0x1133, "Message Notification Server"},
      {0x1134, "Message Access"},
      {0x1135, "GNSS"},
      {0x1136, "GNSS Server"},
      {0x1137, "3D Display"},
      {0x1138, "3D Glasses"},
      {0x1139, "3D Synchronization"},
      {0x113a, "Multi-Profile Specification"},
      {0x113b, "Multi-Profile Specification Service"},
      {0x113c, "Calendar, Task and Notes Access"},
      {0x113d, "Calendar, Task and Notes Notification"},
      {0x113e, "Calendar, Task and Notes"},
      {0x1200, "PnP Information"},
      {0x1201, "Generic Networking"},
      {0x1202, "Generic File Transfer"},
      {0x1203, "Generic Audio"},
      {0x1204, "Generic Telephony"},
      {0x1205, "UPnP Service"},
      {0x1206, "UPnP IP Service"},
      {0x1300, "UPnP IP PAN"},
      {0x1301, "UPnP IP LAP"},
      {0x1302, "UPnP L2CAP"},
      {0x1303, "Video Source"},
      {0x1304, "Video Sink"},
      {0x1305, "Video Distribution"},
      {0x1400, "Health Device"},
      {0x1401, "Health Device Source"},
      {0x1402, "Health Device Sink"},
      {0x1800, "Generic Access"},
      {0x1801, "Generic Attribute"},
      {0x1802, "Immediate Alert"},
      {0x1803, "Link Loss"},
      {0x1804, "Tx Power"},
      {0x1805, "Current Time"},
      {0x1806, "Reference Time Update"},
      {0x1807, "Next DST Change"},
      {0x1808, "Glucose"},
      {0x1809, "Health Thermometer"},
      {0x180a, "Device Information"},
      {0x180d, "Heart Rate"},
      {0x180e, "Phone Alert Status"},
      {0x180f, "Battery"},
      {0x1810, "Blood Pressure"},
      {0x1811, "Alert Notification"},
      {0x1812, "Human Interface Device"},
      {0x1813, "Scan Parameters"},
      {0x1814, "Running Speed and Cadence"},
      {0x1815, "Automation IO"},
      {0x1816, "Cycling Speed and Cadence"},
      {0x1818, "Cycling Power"},
      {0x1819, "Location and Navigation"},
      {0x181a, "Environmental Sensing"},
      {0x181b, "Body Composition"},
      {0x181c, "User Data"},
      {0x181d, "Weight Scale"},
      {0x181e, "Bond Management"},
      {0x181f, "Continuous Glucose Monitoring"},
      {0x1820, "Internet Protocol Support"},
      {0x1821, "Indoor Positioning"},
      {0x1822, "Pulse Oximeter"},
      {0x1823, "HTTP Proxy"},
      {0x1824, "Transport Discovery"},
      {0x1825, "Object Transfer"},
      {0x1826, "Fitness Machine"},
      {0x1827, "Mesh Provisioning"},
      {0x1828, "Mesh Proxy"},
      {0x1829, "Reconnection Configuration"},
      {0x183a, "Insulin Delivery"},
      {0x183b, "Binary Sensor"},
      {0x183c, "Emergency Configuration"},
      {0x183d, "Authorization Control"},
      {0x183e, "Physical Activity Monitor"},
      {0x183f, "Elapsed Time"},
      {0x1840, "Generic Health Sensor"},
      {0x1843, "Audio Input Control"},
      {0x1844, "Volume Control"},
      {0x1845, "Volume Offset Control"},
      {0x1846, "Coordinated Set Identification"},
      {0x1847, "Device Time"},
      {0x1848, "Media Control"},
      {0x1849, "Generic Media Control"},
      {0x184a, "Constant Tone Extension"},
      {0x184b, "Telephone Bearer"},
      {0x184c, "Generic Telephone Bearer"},
      {0x184d, "Microphone Control"},
      {0x184e, "Audio Stream Control"},
      {0x184f, "Broadcast Audio Scan"},
      {0x1850, "Published Audio Capabilities"},
      {0x1851, "Basic Audio Announcement"},
      {0x1852, "Broadcast Audio Announcement"},
      {0x1853, "Common Audio"},
      {0x1854, "Hearing Access"},
      {0x1855, "Telephony and Media Audio"},
      {0x1856, "Public Broadcast Announcement"},
      {0x1857, "Electronic Shelf Label"},
      {0x1858, "Gaming Audio"},
      {0x1859, "Mesh Proxy Solicitation"},
      {0x185a, "Industrial Measurement Device"},
      {0x185b, "Ranging"},
      {0x185c, "HID ISO"},
      {0x185d, "Cookware"},
      {0x185e, "Voice Assistant"},
      {0x185f, "Generic Voice Assistant"},
      {0x1860, "Tire Pressure Monitoring System"},
  });

  static_assert(
      std::ranges::is_sorted(kServiceNames, {}, &ServiceName::first), "kServiceNames must stay sorted for lookup"
  );

  // Vendor UUIDs common enough on the desktop that the raw form is worth avoiding.
  using VendorName = std::pair<std::string_view, std::string_view>;

  constexpr auto kVendorNames = std::to_array<VendorName>({
      {"6e400001-b5a3-f393-e0a9-e50e24dcca9e", "Nordic UART"},
      {"7905f431-b5ce-4e99-a40f-4b1e122d00d0", "Apple Notification Center"},
      {"89d3502b-0f36-433a-8ef4-c502ad55f8dc", "Apple Media"},
      {"9fa480e0-4967-4542-9390-d343dc5d04ae", "Apple Continuity"},
      {"d0611e78-bbb4-4591-a5f8-487910ae4366", "Apple Continuity"},
  });

  std::string toLower(std::string_view uuid) {
    std::string out(uuid);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  }

  std::optional<std::uint16_t> parseHex16(std::string_view text) {
    std::uint16_t value = 0;
    for (const char c : text) {
      std::uint16_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint16_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint16_t>(c - 'a' + 10);
      } else {
        return std::nullopt;
      }
      value = static_cast<std::uint16_t>((value << 4U) | digit);
    }
    return value;
  }

} // namespace

std::optional<std::uint16_t> bluetoothUuidShortId(std::string_view uuid) {
  const std::string lower = toLower(uuid);

  // BlueZ always sends the 128-bit form; SDP records and config files also use the
  // bare 16-bit form.
  if (lower.size() == 4) {
    return parseHex16(lower);
  }
  if (lower.size() != 36 || !lower.ends_with(kBaseSuffix)) {
    return std::nullopt;
  }
  // The first four digits must be zero for the value to fit 16 bits.
  if (lower.compare(0, 4, "0000") != 0) {
    return std::nullopt;
  }
  return parseHex16(std::string_view(lower).substr(4, 4));
}

std::optional<std::string_view> bluetoothServiceName(std::string_view uuid) {
  if (const auto shortId = bluetoothUuidShortId(uuid); shortId.has_value()) {
    const auto it = std::ranges::lower_bound(kServiceNames, *shortId, {}, &ServiceName::first);
    if (it != kServiceNames.end() && it->first == *shortId) {
      return it->second;
    }
    return std::nullopt;
  }

  const std::string lower = toLower(uuid);
  for (const auto& [vendorUuid, name] : kVendorNames) {
    if (lower == vendorUuid) {
      return name;
    }
  }
  return std::nullopt;
}

std::string bluetoothServiceLabel(std::string_view uuid) {
  if (uuid.empty()) {
    return {};
  }
  if (const auto name = bluetoothServiceName(uuid); name.has_value()) {
    return std::string(*name);
  }
  if (const auto shortId = bluetoothUuidShortId(uuid); shortId.has_value()) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04x", *shortId);
    return buf;
  }
  return toLower(uuid);
}
