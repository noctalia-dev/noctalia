#pragma once

namespace noctalia::sysmon {

  enum class Stat {
    CpuUsage,
    CpuTemp,
    GpuTemp,
    GpuUsage,
    GpuVram,
    RamUsed,
    RamPct,
    SwapPct,
    DiskUsedPct,
    DiskUsed,
    DiskFreePct,
    DiskFree,
    NetRx,
    NetTx
  };

  struct ThresholdProfile {
    double activityDefault = 50.0;
    double criticalDefault = 100.0;
    double minValue = 0.0;
    double maxValue = 100.0;
    double step = 1.0;
  };

  [[nodiscard]] inline constexpr ThresholdProfile thresholdProfile(Stat stat) {
    switch (stat) {
    case Stat::CpuUsage:
      return ThresholdProfile{.activityDefault = 50.0, .criticalDefault = 90.0};
    case Stat::CpuTemp:
    case Stat::GpuTemp:
      return ThresholdProfile{.activityDefault = 60.0, .criticalDefault = 85.0};
    case Stat::GpuUsage:
      return ThresholdProfile{.activityDefault = 50.0, .criticalDefault = 95.0};
    case Stat::GpuVram:
      return ThresholdProfile{.activityDefault = 50.0, .criticalDefault = 90.0};
    case Stat::RamUsed:
    case Stat::RamPct:
      return ThresholdProfile{.activityDefault = 60.0, .criticalDefault = 90.0};
    case Stat::SwapPct:
      return ThresholdProfile{.activityDefault = 20.0, .criticalDefault = 80.0};
    case Stat::DiskUsedPct:
    case Stat::DiskUsed:
    case Stat::DiskFreePct:
    case Stat::DiskFree:
      return ThresholdProfile{.activityDefault = 80.0, .criticalDefault = 95.0};
    case Stat::NetRx:
    case Stat::NetTx:
      return ThresholdProfile{
          .activityDefault = 1.0, .criticalDefault = 50.0, .minValue = 0.0, .maxValue = 100.0, .step = 0.1
      };
    }
    return ThresholdProfile{};
  }

} // namespace noctalia::sysmon
