#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class Urgency : uint8_t {
  Low = 0,
  Normal = 1,
  Critical = 2,
};

// org.freedesktop.Notifications close reason codes
enum class CloseReason : uint32_t {
  Expired = 1,
  Dismissed = 2,
  ClosedByCall = 3,
};

enum class NotificationOrigin : uint8_t {
  External = 0,
  Internal = 1,
};

enum class NotificationDndPolicy : uint8_t {
  Respect = 0,
  ShowToast = 1,
  Bypass = 2,
};

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using WallClock = std::chrono::system_clock;
using WallTimePoint = WallClock::time_point;

struct NotificationImageData {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t rowStride = 0;
  bool hasAlpha = true;
  std::int32_t bitsPerSample = 8;
  std::int32_t channels = 4;
  std::vector<std::uint8_t> data;

  bool operator==(const NotificationImageData&) const = default;
};

struct Notification {
  uint32_t id;
  NotificationOrigin origin;
  NotificationDndPolicy dndPolicy = NotificationDndPolicy::Respect;
  bool transient = false;
  std::string appName;
  std::string summary;
  std::string body;
  int32_t timeout;
  Urgency urgency;
  std::vector<std::string> actions; // pairs: [key, label, key, label, ...]
  std::optional<std::string> icon;
  std::optional<NotificationImageData> imageData;
  std::optional<std::string> category;
  std::optional<std::string> desktopEntry;
  TimePoint receivedTime;              // add/replace time used for duplicate burst suppression
  std::optional<TimePoint> expiryTime; // absent = never expires
  /// Wall-clock receive time (history UI, persistence). Optional for pre-migration / corrupt rows.
  std::optional<WallTimePoint> receivedWallClock;
  /// Wall-clock expiry when timeout-based; mirrors expiryTime where applicable.
  std::optional<WallTimePoint> expiryWallClock;
};
