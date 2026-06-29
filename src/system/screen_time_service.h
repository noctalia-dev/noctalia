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

class WaylandConnection;

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
  std::vector<ScreenTimeChartSeries> chartSeries;
  std::vector<ScreenTimeAppUsage> apps;
};

class ScreenTimeService {
public:
  void initialize(WaylandConnection* wayland);
  void shutdown();

  void onFocusChange();
  void setChangeCallback(std::function<void()> callback);
  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }

  [[nodiscard]] ScreenTimeSnapshot snapshot(int rangeDays = 1);

  void clearAll();

private:
  struct DayRecord {
    std::unordered_map<std::string, std::chrono::seconds> apps;
    std::unordered_map<std::string, std::array<std::chrono::seconds, 24>> appHourly;
    std::array<std::chrono::seconds, 24> hourly{};
  };

  void tick();
  void flushActiveSession(std::chrono::steady_clock::time_point now);
  void ensureCurrentDayLocked(std::chrono::system_clock::time_point now);
  void load();
  void save();
  void pruneOldDaysLocked();
  [[nodiscard]] const DayRecord* dayRecordForKey(const std::string& dayKey) const;
  [[nodiscard]] std::vector<std::string> dayKeysForRange(int rangeDays) const;
  [[nodiscard]] std::string appKeyForActive() const;
  [[nodiscard]] static std::string localDayKey(std::chrono::system_clock::time_point tp);
  [[nodiscard]] static std::string shortDayLabel(const std::string& dayKey);
  [[nodiscard]] static int localHour(std::chrono::system_clock::time_point tp);
  [[nodiscard]] static DayRecord materializeDayForCharts(const DayRecord& source);
  [[nodiscard]] static std::chrono::seconds sumApps(const DayRecord& day);
  [[nodiscard]] static std::chrono::seconds sumHourly(const DayRecord& day);
  [[nodiscard]] static std::chrono::seconds appSecondsForKey(const DayRecord& day, const std::string& appKey);
  static void distributeSecondsAcrossHourly(
      std::chrono::seconds amount, const DayRecord& profile, std::vector<std::chrono::seconds>& buckets
  );

  WaylandConnection* m_wayland = nullptr;
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
};
