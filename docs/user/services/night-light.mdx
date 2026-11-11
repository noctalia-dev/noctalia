---
title: Night Light
description: Night light color temperature scheduling.
sidebar:
  order: 8
---

Applies color temperature shifts using the Wayland `wlr-gamma-control` protocol. Supported on compositors that implement gamma control (Hyprland, Sway, niri, etc.). Follows the [`[location]`](/noctalia/services/location/) schedule when enabled; use `force` to ignore the schedule.

```toml
[nightlight]
enabled              = false
force                = false   # hold night temperature, ignoring the schedule

temperature_day   = 6500       # Kelvin (must be > temperature_night by at least 100)
temperature_night = 4000       # Kelvin
```

## How the fade works

Day and night are separated by a **one-hour fade** centered on sunrise and sunset, so the temperature starts moving 30 minutes before the boundary and settles 30 minutes after it. The scheduled time is the midpoint of the fade, not its start.

The fade is anchored to the wall clock, not to when Noctalia started. The temperature you get is a function of how far into the fade window the current time is, so starting the shell in the middle of a sunset lands on the right temperature immediately rather than beginning a fresh fade.

**Only the schedule fades.** Anything else applies at once:

- Startup applies the current scheduled (or forced) temperature immediately.
- Enabling, disabling, or toggling `force` (from settings or [IPC](/noctalia/ipc/system-controls/#night-light)) snaps straight to the new temperature.
- Disabling night light releases gamma control, which restores the compositor's native output instantly.

## Scheduled Brightness

Scheduled brightness adjusts screen brightness using the same day/night schedule from [`[location]`](/noctalia/services/location/) (sunrise/sunset or custom times). It is independent of night light and works on DDC and backlight displays even where gamma control is unavailable.

```toml
[brightness]
enable_scheduled_brightness = false
scheduled_day_brightness    = 1.0   # target brightness during day phase (0.0–1.0)
scheduled_night_brightness  = 0.5   # target brightness during night phase (0.0–1.0)
```

### Gradient fade behavior

Unlike the one-hour, boundary-centred fade of night light, scheduled brightness fades only in the **30 minutes before** each boundary:

- Fade begins 30 minutes before the boundary (sunrise or sunset).
- At the boundary, brightness reaches the target phase value (`scheduled_day_brightness` at sunrise, `scheduled_night_brightness` at sunset) and stays there until the next fade window.
- Inside a fade window, each display ramps linearly from its brightness at fade start to the target, moving 1% at a time (at most one write every 2 seconds). Changing a monitor's brightness in Noctalia during the fade re-anchors its ramp to the new value for the remaining time; a display mode change (for example a game switching HDR) does not, the monitor rejoins its ramp. If a day/night phase is shorter than the fade window, the next fade starts immediately at the boundary from the current level. Outside it, no writes happen unless settings change or displays appear/disappear.

If your brightness before sunset is higher than `scheduled_day_brightness`, that level is restored right after sunrise: the day target never drops below your pre-sunset brightness (still clamped to `minimum_brightness`).

## Related IPC

For night light commands, see [System Controls → Night light](/noctalia/ipc/system-controls/#night-light).
