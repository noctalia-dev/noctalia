---
title: Battery
description: Battery warning thresholds and per-device battery configuration.
sidebar:
  order: 2
---

Battery status uses UPower. The global warning threshold controls the laptop low-battery notifications and the warning color state used by default battery widgets, and it follows the combined charge across all laptop batteries, not any single battery. Peripheral batteries warn only when they have an explicit per-device threshold.

```toml
[battery]
warning_threshold = 10 # 0 disables low-battery warnings

# Warning for a second laptop battery, at that battery's own percent
# See the Multiple laptop batteries section below.
[battery.device."/org/freedesktop/UPower/devices/battery_BAT1"]
warning_threshold = 15

[battery.device."/org/freedesktop/UPower/devices/headset_dev_00_11_22_33_44_55"]
warning_threshold = 10
```

## Multiple laptop batteries

On laptops with more than one battery, the laptop warns once on the combined charge (UPower's display battery), not once per battery, and the notification shows the same combined percentage the bar widget displays. A second battery stays silent on its own. A per-device threshold warns about it individually, always at its own percent, like the `battery_BAT1` example above.

Single-battery laptops are unaffected: the combined charge equals the single battery's charge, so thresholds, escalation levels, and notification text work exactly as before.

## Escalating low-battery warnings

Warnings are level-triggered, not edge-triggered: the shell re-evaluates the live battery level, so you are warned even when the shell starts (or the battery is hot-plugged) already below the threshold - not only when the level crosses downward.

For the **system battery**, warnings escalate as the charge drains:

- At `warning_threshold` (default 10%): a normal "Battery low" notification.
- At 5%: a critical notification.
- At 2%: a persistent critical notification warning that the computer will shut down soon (stays on screen until you charge or dismiss it).

The 5% and 2% escalation levels are fixed and apply only to the system-level warning, which follows the combined charge (UPower's display battery): a second laptop battery draining low does not warn on its own unless it has a per-device threshold. Charging back above a level re-arms it, so re-discharging warns again. Laptop and peripheral batteries with an explicit per-device threshold warn only once at their own configured `warning_threshold`. Setting `warning_threshold = 0` disables all warnings for that battery, including the escalation levels.

System-battery warnings remain visible while Do Not Disturb is enabled, but their notification sound stays muted. Peripheral battery warnings continue to respect Do Not Disturb.

Per-device tables use the same UPower selectors as the battery widget `device` setting. The Settings UI writes full UPower object paths for reported non-system devices. Do not add a per-device table for the default system battery; `[battery].warning_threshold` is its canonical threshold.
