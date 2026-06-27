#include "system/screen_time_service.h"

#include "core/log.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/internal_app_metadata.h"
#include "util/file_utils.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_toplevels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <json.hpp>
#include <optional>
#include <utility>

namespace {

  constexpr Logger kLog("screen-time");
  constexpr int kRetentionDays = 14;
  constexpr auto kTickInterval = std::chrono::seconds(5);

  [[nodiscard]] std::chrono::system_clock::time_point localNow() { return std::chrono::system_clock::now(); }

  [[nodiscard]] std::tm localTm(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm out{};
    localtime_r(&t, &out);
    return out;
  }

  // Match ScreenTimeService::localDayKey(localNow()) — not std::chrono::days on system_clock (UTC).
  [[nodiscard]] std::string localDayKeyForCalendarOffset(int daysAgo) {
    std::tm tm = localTm(localNow());
    tm.tm_hour = 12;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    tm.tm_mday -= daysAgo;
    if (std::mktime(&tm) == -1) {
      tm = localTm(localNow());
    }
    return std::format("{:04d}-{:02d}-{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  }

  // Control center UI limits.
  constexpr std::size_t kMaxListedApps = 12;
  constexpr std::size_t kMaxChartSeries = 5;

  [[nodiscard]] std::string canonicalAppKey(std::string_view appKey) {
    if (const auto sep = appKey.find('\x1f'); sep != std::string::npos) {
      return std::string(appKey.substr(0, sep));
    }
    return std::string(appKey);
  }

  [[nodiscard]] std::string normalizedAppToken(std::string_view value) {
    std::string token;
    token.reserve(value.size());
    for (const unsigned char ch : value) {
      if (ch == '.' || ch == '-' || ch == '_' || std::isspace(ch) != 0) {
        continue;
      }
      token.push_back(static_cast<char>(std::tolower(ch)));
    }
    return token;
  }

  [[nodiscard]] bool normalizedContains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
      return false;
    }
    const std::string hay = normalizedAppToken(haystack);
    const std::string ned = normalizedAppToken(needle);
    return !hay.empty() && hay.contains(ned);
  }

  // Portals, polkit prompts, and other transient helpers should not receive focus credit.
  [[nodiscard]] bool isScreenTimeExcludedAppKey(std::string_view appKey) {
    if (appKey.empty() || appKey == "unknown") {
      return true;
    }
    if (appKey.starts_with("title:")) {
      return false;
    }

    const std::string baseKey = canonicalAppKey(appKey);
    if (baseKey.empty()) {
      return true;
    }
    if (internal_apps::appDefinitionForAppId(baseKey) != nullptr) {
      return true;
    }

    const std::string lower = StringUtils::toLower(baseKey);
    if (lower.starts_with("dev.noctalia.")) {
      return true;
    }

    static constexpr std::string_view kExcludedSubstrings[] = {
        "xdg-desktop-portal", "xdgdesktopportal", "org.freedesktop.portal", "org.freedesktop.policykit", "polkit",
        "gcr-prompter",       "kwallet",          "org.gnome.zenity",
    };
    for (const std::string_view pattern : kExcludedSubstrings) {
      if (lower.contains(pattern) || normalizedContains(lower, pattern)) {
        return true;
      }
    }

    static constexpr std::string_view kExcludedExact[] = {
        "xdg-dbus-proxy",
        "org.freedesktop.dbus",
    };
    for (const std::string_view exact : kExcludedExact) {
      if (lower == exact || normalizedContains(lower, exact)) {
        return true;
      }
    }

    const auto entry = app_identity::findDesktopEntry(
        baseKey, desktopEntries(),
        app_identity::DesktopEntryLookupOptions{.includeHidden = true, .includeNoDisplay = true}
    );
    if (!entry.has_value()) {
      return false;
    }
    if (entry->hidden) {
      return true;
    }
    if (!entry->noDisplay) {
      return false;
    }
    const auto& idLower = entry->idLower;
    return entry->categoriesLower.contains("utility")
        || idLower.contains("portal")
        || idLower.contains("polkit")
        || idLower.contains("auth")
        || idLower.contains("kwallet")
        || idLower.contains("gcr");
  }

  template <typename HourlyMap>
  void pruneExcludedAppKeys(std::unordered_map<std::string, std::chrono::seconds>& apps, HourlyMap& appHourly) {
    for (auto it = apps.begin(); it != apps.end();) {
      if (isScreenTimeExcludedAppKey(it->first)) {
        it = apps.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = appHourly.begin(); it != appHourly.end();) {
      if (isScreenTimeExcludedAppKey(it->first)) {
        it = appHourly.erase(it);
      } else {
        ++it;
      }
    }
  }

  [[nodiscard]] std::string displayNameForAppKey(const std::string& appKey) {
    if (appKey.empty() || appKey == "unknown") {
      return {};
    }
    if (appKey.starts_with("title:")) {
      return appKey.substr(6);
    }
    if (const auto sep = appKey.find('\x1f'); sep != std::string::npos) {
      const std::string embeddedTitle = appKey.substr(sep + 1);
      if (!embeddedTitle.empty()) {
        return embeddedTitle;
      }
    }

    const std::string baseKey = canonicalAppKey(appKey);
    const auto lookupOptions = baseKey.starts_with("steam_app_")
        ? app_identity::DesktopEntryLookupOptions{.includeHidden = true, .includeNoDisplay = true}
        : app_identity::DesktopEntryLookupOptions{};
    if (const auto entry = app_identity::findDesktopEntry(baseKey, desktopEntries(), lookupOptions);
        entry.has_value()) {
      if (!entry->name.empty()) {
        return entry->name;
      }
      if (!entry->genericName.empty()) {
        return entry->genericName;
      }
    }

    const auto entry = app_identity::resolveRunningDesktopEntry(baseKey, desktopEntries());
    if (!entry.name.empty()) {
      return entry.name;
    }
    if (!entry.genericName.empty()) {
      return entry.genericName;
    }
    return baseKey;
  }

} // namespace

std::chrono::seconds ScreenTimeService::sumApps(const DayRecord& day) {
  std::chrono::seconds total{0};
  for (const auto& [_, seconds] : day.apps) {
    total += seconds;
  }
  return total;
}

std::chrono::seconds ScreenTimeService::sumHourly(const DayRecord& day) {
  std::chrono::seconds total{0};
  for (const auto& bucket : day.hourly) {
    total += bucket;
  }
  return total;
}

std::chrono::seconds ScreenTimeService::appSecondsForKey(const DayRecord& day, const std::string& appKey) {
  std::chrono::seconds total{0};
  for (const auto& [storedKey, seconds] : day.apps) {
    if (canonicalAppKey(storedKey) == appKey) {
      total += seconds;
    }
  }
  return total;
}

void ScreenTimeService::distributeSecondsAcrossHourly(
    std::chrono::seconds amount, const DayRecord& profile, std::vector<std::chrono::seconds>& buckets
) {
  if (amount.count() <= 0 || buckets.empty()) {
    return;
  }

  const std::chrono::seconds profileTotal = sumHourly(profile);
  if (profileTotal.count() <= 0) {
    const int hour = std::clamp(localHour(localNow()), 0, 23);
    buckets[static_cast<std::size_t>(hour)] += amount;
    return;
  }

  std::int64_t assigned = 0;
  std::size_t largestHour = 0;
  std::int64_t largestShare = -1;
  for (std::size_t hour = 0; hour < buckets.size() && hour < profile.hourly.size(); ++hour) {
    const std::int64_t share = (amount.count() * profile.hourly[hour].count()) / profileTotal.count();
    buckets[hour] += std::chrono::seconds(share);
    assigned += share;
    if (share > largestShare) {
      largestShare = share;
      largestHour = hour;
    }
  }
  const std::int64_t remainder = amount.count() - assigned;
  if (remainder > 0) {
    buckets[largestHour] += std::chrono::seconds(remainder);
  }
}

ScreenTimeService::DayRecord ScreenTimeService::materializeDayForCharts(const DayRecord& source) {
  DayRecord day = source;
  const std::chrono::seconds appsTotal = sumApps(day);
  std::chrono::seconds hourlyTotal = sumHourly(day);

  if (hourlyTotal < appsTotal) {
    day.hourly.fill(std::chrono::seconds{0});
    for (const auto& [_, buckets] : day.appHourly) {
      for (std::size_t hour = 0; hour < buckets.size() && hour < day.hourly.size(); ++hour) {
        day.hourly[hour] += buckets[hour];
      }
    }
    hourlyTotal = sumHourly(day);
  }

  if (hourlyTotal < appsTotal) {
    day.hourly.fill(std::chrono::seconds{0});
    const int hour = std::clamp(localHour(localNow()), 0, 23);
    day.hourly[static_cast<std::size_t>(hour)] = appsTotal;
  }

  return day;
}

void ScreenTimeService::initialize(WaylandConnection* wayland) {
  m_wayland = wayland;
  const std::string dir = FileUtils::stateDir();
  m_storagePath = dir.empty() ? "screen_time.json" : dir + "/screen_time.json";
  load();
  ensureCurrentDayLocked(localNow());
}

void ScreenTimeService::shutdown() {
  m_tickTimer.stop();
  flushActiveSession(std::chrono::steady_clock::now());
  if (m_dirty) {
    save();
  }
}

void ScreenTimeService::setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

void ScreenTimeService::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (!m_enabled) {
    flushActiveSession(std::chrono::steady_clock::now());
    m_activeAppKey.clear();
    m_activeSince = {};
    if (m_dirty) {
      save();
    }
    m_tickTimer.stop();
  } else {
    onFocusChange();
    if (!m_tickTimer.active()) {
      m_tickTimer.startRepeating(kTickInterval, [this]() { tick(); });
    }
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ScreenTimeService::onFocusChange() {
  if (!m_enabled) {
    return;
  }
  const std::string candidate = appKeyForActive();
  if (isScreenTimeExcludedAppKey(candidate)) {
    // File pickers and portal dialogs briefly take focus; keep crediting the last real app.
    return;
  }
  if (candidate.empty()) {
    flushActiveSession(std::chrono::steady_clock::now());
    m_activeAppKey.clear();
    m_activeSince = {};
    return;
  }
  if (candidate == m_activeAppKey) {
    return;
  }
  flushActiveSession(std::chrono::steady_clock::now());
  m_activeAppKey = candidate;
  m_activeSince = std::chrono::steady_clock::now();
}

void ScreenTimeService::tick() {
  if (!m_enabled) {
    return;
  }
  const bool wasDirty = m_dirty;
  flushActiveSession(std::chrono::steady_clock::now());
  if (wasDirty || m_dirty) {
    save();
  }
}

const ScreenTimeService::DayRecord* ScreenTimeService::dayRecordForKey(const std::string& dayKey) const {
  if (dayKey == m_currentDayKey) {
    return &m_currentDay;
  }
  const auto it = m_days.find(dayKey);
  if (it != m_days.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<std::string> ScreenTimeService::dayKeysForRange(int rangeDays) const {
  std::vector<std::string> keys;
  keys.reserve(static_cast<std::size_t>(rangeDays));
  // Oldest day first so multi-day charts read left-to-right chronologically.
  for (int offset = rangeDays - 1; offset >= 0; --offset) {
    keys.push_back(localDayKeyForCalendarOffset(offset));
  }
  return keys;
}

std::string ScreenTimeService::shortDayLabel(const std::string& dayKey) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (std::sscanf(dayKey.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
    return dayKey;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_isdst = -1;
  if (std::mktime(&tm) == -1) {
    return dayKey;
  }
  static constexpr const char* kWeekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return kWeekdays[tm.tm_wday];
}

ScreenTimeSnapshot ScreenTimeService::snapshot(int rangeDays) {
  if (!m_enabled) {
    return {};
  }
  flushActiveSession(std::chrono::steady_clock::now());

  rangeDays = std::clamp(rangeDays, 1, kRetentionDays);
  ScreenTimeSnapshot snapshot;
  snapshot.rangeDays = rangeDays;
  snapshot.hourlyBuckets = rangeDays == 1;

  const std::vector<std::string> dayKeys = dayKeysForRange(rangeDays);
  std::unordered_map<std::string, std::chrono::seconds> mergedApps;

  std::optional<DayRecord> materializedToday;
  const DayRecord* todayForCharts = nullptr;
  if (snapshot.hourlyBuckets) {
    snapshot.buckets.resize(24);
    const DayRecord* today = dayRecordForKey(dayKeys.front());
    if (today == nullptr && !m_currentDayKey.empty()) {
      today = dayRecordForKey(m_currentDayKey);
    }
    if (today != nullptr) {
      materializedToday = materializeDayForCharts(*today);
      todayForCharts = &*materializedToday;
      for (std::size_t hour = 0; hour < todayForCharts->hourly.size(); ++hour) {
        snapshot.buckets[hour] = todayForCharts->hourly[hour];
        snapshot.total += todayForCharts->hourly[hour];
      }
      for (const auto& [appKey, seconds] : todayForCharts->apps) {
        if (seconds.count() > 0 && !isScreenTimeExcludedAppKey(appKey)) {
          mergedApps[canonicalAppKey(appKey)] += seconds;
        }
      }
    }
  } else {
    snapshot.buckets.resize(dayKeys.size());
    snapshot.bucketLabels.reserve(dayKeys.size());
    for (std::size_t dayIndex = 0; dayIndex < dayKeys.size(); ++dayIndex) {
      snapshot.bucketLabels.push_back(shortDayLabel(dayKeys[dayIndex]));
      const DayRecord* day = dayRecordForKey(dayKeys[dayIndex]);
      if (day == nullptr) {
        continue;
      }
      std::chrono::seconds dayTotal{0};
      for (const auto& [appKey, seconds] : day->apps) {
        if (seconds.count() <= 0 || isScreenTimeExcludedAppKey(appKey)) {
          continue;
        }
        dayTotal += seconds;
        mergedApps[canonicalAppKey(appKey)] += seconds;
      }
      snapshot.buckets[dayIndex] = dayTotal;
      snapshot.total += dayTotal;
    }
  }

  snapshot.apps.reserve(mergedApps.size());
  for (const auto& [appKey, seconds] : mergedApps) {
    std::string displayName = displayNameForAppKey(appKey);
    if (displayName.empty()) {
      displayName = appKey;
    }
    snapshot.apps.push_back(
        ScreenTimeAppUsage{
            .appKey = appKey,
            .displayName = std::move(displayName),
            .total = seconds,
        }
    );
  }

  std::ranges::sort(snapshot.apps, [](const ScreenTimeAppUsage& a, const ScreenTimeAppUsage& b) {
    if (a.total != b.total) {
      return a.total > b.total;
    }
    return a.displayName < b.displayName;
  });

  std::vector<ScreenTimeAppUsage> rankedApps = std::move(snapshot.apps);
  // Chart: top kMaxChartSeries. List: up to kMaxListedApps.
  snapshot.apps.assign(
      rankedApps.begin(), rankedApps.begin() + static_cast<std::ptrdiff_t>(std::min(rankedApps.size(), kMaxListedApps))
  );

  const auto fillSeriesBuckets = [&](ScreenTimeChartSeries& series, const std::string& appKey) {
    if (snapshot.hourlyBuckets) {
      if (todayForCharts == nullptr) {
        return;
      }
      bool matchedHourly = false;
      for (const auto& [storedKey, hourly] : todayForCharts->appHourly) {
        if (canonicalAppKey(storedKey) != appKey) {
          continue;
        }
        matchedHourly = true;
        for (std::size_t hour = 0; hour < hourly.size() && hour < series.buckets.size(); ++hour) {
          series.buckets[hour] += hourly[hour];
        }
      }
      if (!matchedHourly) {
        const std::chrono::seconds appTotal = appSecondsForKey(*todayForCharts, appKey);
        if (appTotal.count() > 0) {
          distributeSecondsAcrossHourly(appTotal, *todayForCharts, series.buckets);
        }
      }
      return;
    }
    for (std::size_t dayIndex = 0; dayIndex < dayKeys.size(); ++dayIndex) {
      const DayRecord* day = dayRecordForKey(dayKeys[dayIndex]);
      if (day == nullptr) {
        continue;
      }
      for (const auto& [storedKey, seconds] : day->apps) {
        if (canonicalAppKey(storedKey) == appKey) {
          series.buckets[dayIndex] += seconds;
        }
      }
    }
  };

  const std::size_t chartCount = std::min(rankedApps.size(), kMaxChartSeries);
  snapshot.chartSeries.reserve(chartCount);
  for (std::size_t i = 0; i < chartCount; ++i) {
    const auto& app = rankedApps[i];
    ScreenTimeChartSeries series{
        .appKey = app.appKey,
        .displayName = app.displayName,
        .buckets = std::vector<std::chrono::seconds>(snapshot.buckets.size()),
        .total = app.total,
        .chartColor = app.chartColor,
    };
    fillSeriesBuckets(series, app.appKey);
    snapshot.chartSeries.push_back(std::move(series));
  }

  return snapshot;
}

void ScreenTimeService::clearAll() {
  m_activeAppKey.clear();
  m_activeSince = {};
  m_days.clear();
  m_currentDay = {};
  m_currentDayKey.clear();
  ensureCurrentDayLocked(localNow());
  m_dirty = true;
  save();
  if (m_enabled) {
    onFocusChange();
  }
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ScreenTimeService::flushActiveSession(std::chrono::steady_clock::time_point now) {
  if (m_activeAppKey.empty()
      || m_activeSince == std::chrono::steady_clock::time_point{}
      || isScreenTimeExcludedAppKey(m_activeAppKey)) {
    return;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_activeSince);
  if (elapsed.count() <= 0) {
    return;
  }

  ensureCurrentDayLocked(localNow());

  m_currentDay.apps[m_activeAppKey] += elapsed;
  const int hour = localHour(localNow());
  if (hour >= 0 && hour < static_cast<int>(m_currentDay.hourly.size())) {
    m_currentDay.hourly[static_cast<std::size_t>(hour)] += elapsed;
    m_currentDay.appHourly[m_activeAppKey][static_cast<std::size_t>(hour)] += elapsed;
  }

  m_activeSince = now;
  m_dirty = true;

  if (m_changeCallback) {
    m_changeCallback();
  }
}

void ScreenTimeService::ensureCurrentDayLocked(std::chrono::system_clock::time_point now) {
  const std::string dayKey = localDayKey(now);
  if (!m_currentDayKey.empty() && dayKey == m_currentDayKey) {
    return;
  }

  if (!m_currentDayKey.empty()) {
    auto& stored = m_days[m_currentDayKey];
    for (const auto& [key, seconds] : m_currentDay.apps) {
      stored.apps[key] += seconds;
    }
    for (const auto& [key, buckets] : m_currentDay.appHourly) {
      auto& storedBuckets = stored.appHourly[key];
      for (std::size_t hour = 0; hour < buckets.size(); ++hour) {
        storedBuckets[hour] += buckets[hour];
      }
    }
    for (std::size_t hour = 0; hour < m_currentDay.hourly.size(); ++hour) {
      stored.hourly[hour] += m_currentDay.hourly[hour];
    }
    m_currentDay = {};
  }

  m_currentDayKey = dayKey;
  const auto it = m_days.find(dayKey);
  if (it != m_days.end()) {
    m_currentDay = it->second;
    m_days.erase(it);
  } else {
    m_currentDay = {};
  }

  pruneOldDaysLocked();
  m_dirty = true;
}

void ScreenTimeService::load() {
  std::ifstream file(m_storagePath);
  if (!file.is_open()) {
    return;
  }

  bool prunedStored = false;
  try {
    const auto json = nlohmann::json::parse(file);
    if (!json.is_object()) {
      return;
    }
    for (const auto& [dayKey, dayJson] : json.items()) {
      if (!dayJson.is_object()) {
        continue;
      }
      DayRecord record;
      if (dayJson.contains("apps") && dayJson["apps"].is_object()) {
        for (const auto& [appKey, seconds] : dayJson["apps"].items()) {
          if (seconds.is_number_integer()) {
            record.apps[appKey] = std::chrono::seconds(seconds.template get<std::int64_t>());
          }
        }
      }
      if (dayJson.contains("hourly") && dayJson["hourly"].is_array()) {
        std::size_t hour = 0;
        for (const auto& value : dayJson["hourly"]) {
          if (hour >= record.hourly.size()) {
            break;
          }
          if (value.is_number_integer()) {
            record.hourly[hour] = std::chrono::seconds(value.template get<std::int64_t>());
          }
          ++hour;
        }
      }
      if (dayJson.contains("app_hourly") && dayJson["app_hourly"].is_object()) {
        for (const auto& [appKey, bucketsJson] : dayJson["app_hourly"].items()) {
          if (!bucketsJson.is_array()) {
            continue;
          }
          auto buckets = std::array<std::chrono::seconds, 24>{};
          std::size_t hour = 0;
          for (const auto& value : bucketsJson) {
            if (hour >= buckets.size()) {
              break;
            }
            if (value.is_number_integer()) {
              buckets[hour] = std::chrono::seconds(value.template get<std::int64_t>());
            }
            ++hour;
          }
          record.appHourly.emplace(appKey, buckets);
        }
      }
      const std::size_t appsBefore = record.apps.size();
      const std::size_t hourlyBefore = record.appHourly.size();
      pruneExcludedAppKeys(record.apps, record.appHourly);
      if (record.apps.size() != appsBefore || record.appHourly.size() != hourlyBefore) {
        prunedStored = true;
      }
      m_days.emplace(dayKey, std::move(record));
    }
  } catch (const nlohmann::json::exception& e) {
    kLog.warn("failed to parse {}: {}", m_storagePath, e.what());
  }
  const std::size_t currentAppsBefore = m_currentDay.apps.size();
  const std::size_t currentHourlyBefore = m_currentDay.appHourly.size();
  pruneExcludedAppKeys(m_currentDay.apps, m_currentDay.appHourly);
  if (prunedStored
      || m_currentDay.apps.size() != currentAppsBefore
      || m_currentDay.appHourly.size() != currentHourlyBefore) {
    m_dirty = true;
  }
}

void ScreenTimeService::save() {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(m_storagePath).parent_path(), ec);

  nlohmann::json root = nlohmann::json::object();
  auto writeDay = [](const std::string& dayKey, const DayRecord& day) {
    nlohmann::json dayJson;
    nlohmann::json apps = nlohmann::json::object();
    for (const auto& [appKey, seconds] : day.apps) {
      apps[appKey] = seconds.count();
    }
    dayJson["apps"] = std::move(apps);
    nlohmann::json hourly = nlohmann::json::array();
    for (const auto& bucket : day.hourly) {
      hourly.push_back(bucket.count());
    }
    dayJson["hourly"] = std::move(hourly);
    nlohmann::json appHourly = nlohmann::json::object();
    for (const auto& [appKey, buckets] : day.appHourly) {
      nlohmann::json bucketJson = nlohmann::json::array();
      for (const auto& bucket : buckets) {
        bucketJson.push_back(bucket.count());
      }
      appHourly[appKey] = std::move(bucketJson);
    }
    dayJson["app_hourly"] = std::move(appHourly);
    return std::pair{dayKey, std::move(dayJson)};
  };

  for (const auto& [dayKey, day] : m_days) {
    auto [key, json] = writeDay(dayKey, day);
    root[key] = std::move(json);
  }
  if (!m_currentDayKey.empty()) {
    auto [key, json] = writeDay(m_currentDayKey, m_currentDay);
    root[key] = std::move(json);
  }

  std::ofstream file(m_storagePath, std::ios::trunc);
  if (!file.is_open()) {
    kLog.warn("failed to write {}", m_storagePath);
    return;
  }
  file << root.dump(2) << '\n';
  m_dirty = false;
}

void ScreenTimeService::pruneOldDaysLocked() {
  if (m_days.size() <= static_cast<std::size_t>(kRetentionDays)) {
    return;
  }

  std::vector<std::string> keys;
  keys.reserve(m_days.size());
  for (const auto& [key, _] : m_days) {
    if (key != m_currentDayKey) {
      keys.push_back(key);
    }
  }
  std::ranges::sort(keys);
  while (m_days.size() > static_cast<std::size_t>(kRetentionDays) && !keys.empty()) {
    m_days.erase(keys.front());
    keys.erase(keys.begin());
  }
}

std::string ScreenTimeService::appKeyForActive() const {
  if (m_wayland == nullptr) {
    return {};
  }
  const auto active = m_wayland->activeToplevel();
  if (!active.has_value()) {
    return {};
  }
  if (!active->appId.empty()) {
    return active->appId;
  }
  if (!active->title.empty()) {
    return "title:" + active->title;
  }
  return "unknown";
}

std::string ScreenTimeService::localDayKey(std::chrono::system_clock::time_point tp) {
  const std::tm tm = localTm(tp);
  return std::format("{:04d}-{:02d}-{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

int ScreenTimeService::localHour(std::chrono::system_clock::time_point tp) { return localTm(tp).tm_hour; }
