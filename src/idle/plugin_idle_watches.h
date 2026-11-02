#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class WaylandConnection;
struct ext_idle_notification_v1;

// Idle notifications requested by plugin scripts through noctalia.idle.watch(). Each watch owns one
// ext_idle_notification_v1 and reports idled/resumed transitions through its delivery callback.
// Main thread only: watches arrive as script side effects and are released when their Luau host dies.
class PluginIdleWatches {
public:
  using Delivery = std::function<void(bool idle)>;

  PluginIdleWatches() = default;
  ~PluginIdleWatches();

  PluginIdleWatches(const PluginIdleWatches&) = delete;
  PluginIdleWatches& operator=(const PluginIdleWatches&) = delete;

  void initialize(WaylandConnection& wayland);
  // Re-arms every watch once the idle notifier and seat are bound (they may arrive late).
  void onCapabilitiesReady();
  // D-Bus screensaver inhibits. While inhibited, watches that honor inhibitors do not report idle.
  void setScreenSaverInhibited(bool inhibited);

  void watch(std::uint64_t hostId, int watchId, std::uint32_t timeoutMs, bool ignoreInhibitors, Delivery delivery);
  void unwatch(std::uint64_t hostId, int watchId);
  void release(std::uint64_t hostId);

  static void handleIdled(void* data, ext_idle_notification_v1* notification);
  static void handleResumed(void* data, ext_idle_notification_v1* notification);

private:
  struct Watch {
    PluginIdleWatches* owner = nullptr;
    std::uint64_t hostId = 0;
    int watchId = 0;
    std::uint32_t timeoutMs = 0;
    bool ignoreInhibitors = false;
    Delivery delivery;
    ext_idle_notification_v1* notification = nullptr;
    // Last state reported to the script.
    bool reportedIdle = false;
    // The compositor reported idle while a D-Bus inhibit suppressed it.
    bool idledWhileInhibited = false;
  };

  void arm(Watch& watch);
  static void disarm(Watch& watch);
  void report(Watch& watch, bool idle);

  WaylandConnection* m_wayland = nullptr;
  std::vector<std::unique_ptr<Watch>> m_watches;
  bool m_screenSaverInhibited = false;
  bool m_warnedInputIdleUnsupported = false;
};
