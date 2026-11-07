#pragma once

#include "core/timer_manager.h"
#include "ui/palette.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class CompositorPlatform;

struct ScreenTimeAppUsage {
  std::string appKey;
  std::string displayName;
  std::chrono::seconds total{0};
  ColorSpec chartColor = clearColorSpec();
};

struct ScreenTimeChartSeries {
  std::string appKey;
  std::string displayName;
  std::vector<std::chrono::seconds> buckets;
  std::chrono::seconds total{0};
  ColorSpec chartColor = clearColorSpec();
};

struct ScreenTimeSnapshot {
  int rangeDays = 1;
  bool hourlyBuckets = true;
  std::chrono::seconds total{0};
  std::vector<std::chrono::seconds> buckets;
  std::vector<std::string> bucketLabels;
  // Populated for multi-day charts (one ISO day key per bucket). Empty when hourly.
  std::vector<std::string> bucketDayKeys;
  std::vector<ScreenTimeChartSeries> chartSeries;
  std::vector<ScreenTimeAppUsage> apps;
};

class ScreenTimeService {
public:
  void initialize(CompositorPlatform* platform);
  void shutdown();

  void onFocusChange();
  void setChangeCallback(std::function<void()> callback);
  void setEnabled(bool enabled);
  void setSessionLocked(bool locked);
  void setSuspendPaused(bool paused);
  bool enabled() const noexcept { return m_enabled; }

  ScreenTimeSnapshot snapshot(int rangeDays = 1);
  // Hourly + apps view for a single stored day (`YYYY-MM-DD`). Empty if unknown.
  ScreenTimeSnapshot snapshotForDay(const std::string& dayKey);
  static std::string dayDisplayName(const std::string& dayKey);

  void clearAll();

private:
  struct DayRecord {
    std::unordered_map<std::string, std::chrono::seconds> apps;
    std::unordered_map<std::string, std::array<std::chrono::seconds, 24>> appHourly;
    std::array<std::chrono::seconds, 24> hourly{};
  };
  void pauseTracking();
  void resumeTracking();
  void scheduleNextTick();
  void tick();
  void flushActiveSession(std::chrono::steady_clock::time_point now);
  void ensureCurrentDayLocked(std::chrono::system_clock::time_point now);
  void load();
  void save();
  void pruneOldDaysLocked();
  const DayRecord* dayRecordForKey(const std::string& dayKey) const;
  std::vector<std::string> dayKeysForRange(int rangeDays) const;
  std::string appKeyForActive() const;
  ScreenTimeSnapshot buildHourlySnapshot(const DayRecord& day) const;
  static std::string localDayKey(std::chrono::system_clock::time_point tp);
  static std::string shortDayLabel(const std::string& dayKey);
  static int localHour(std::chrono::system_clock::time_point tp);
  static DayRecord materializeDayForCharts(const DayRecord& source);
  static std::chrono::seconds sumApps(const DayRecord& day);
  static std::chrono::seconds sumHourly(const DayRecord& day);
  static std::chrono::seconds appSecondsForKey(const DayRecord& day, const std::string& appKey);
  static void distributeSecondsAcrossHourly(
      std::chrono::seconds amount, const DayRecord& profile, std::vector<std::chrono::seconds>& buckets
  );

  CompositorPlatform* m_platform = nullptr;
  std::function<void()> m_changeCallback;
  Timer m_tickTimer;
  std::string m_storagePath;
  std::string m_currentDayKey;
  DayRecord m_currentDay;
  std::unordered_map<std::string, DayRecord> m_days;
  std::string m_activeAppKey;
  std::chrono::steady_clock::time_point m_activeSince;
  bool m_dirty = false;
  bool m_enabled = false;
  bool m_sessionLocked = false;
  bool m_suspendPaused = false;
};
