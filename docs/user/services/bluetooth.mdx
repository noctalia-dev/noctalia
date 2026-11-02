---
title: Bluetooth
description: Connecting trusted devices when Bluetooth turns on, and connection notifications.
sidebar:
  order: 3
---

Bluetooth uses BlueZ. Pairing, trust, and per-device connection controls live in the [Control Center Bluetooth tab](/noctalia/control-center/#bluetooth); the options below change what the service does on its own. Configure under **Settings → Services → Bluetooth**.

```toml
[bluetooth]
connect_when_enabled     = false # connect paired, trusted devices when Bluetooth turns on, at startup, and after resume
connection_notifications = false # notify when a device connects or disconnects on its own, or a connect you start fails
```

| Option                     | Type | Default | Description                                                                                                      |
| -------------------------- | ---- | ------- | ---------------------------------------------------------------------------------------------------------------- |
| `connect_when_enabled`     | bool | `false` | Connect each paired, trusted device that is not connected when Bluetooth turns on, at startup, and after resume. |
| `connection_notifications` | bool | `false` | Show a notification when a device connects or disconnects without you acting on it in Noctalia.                  |

## Reconnecting devices at startup

By default Noctalia does not dial out to devices: BlueZ and the devices handle reconnection, and most trusted headsets, keyboards, and mice reconnect on their own when they power on. Some devices instead wait for the computer to connect to them, so after a reboot, a new login, or turning Bluetooth back on they stay disconnected until you connect them from the Bluetooth tab. For those, turn on **Connect Devices When Bluetooth Turns On** or set:

```toml
[bluetooth]
connect_when_enabled = true
```

When Bluetooth turns on (toggled on, at startup, or after resume), Noctalia then connects each paired, trusted device that is not already connected, once and one device at a time. Only trusted devices are included: devices paired from the Bluetooth tab are trusted, and the tab's **Auto-reconnect** switch controls trust for each device. A device that is switched off holds the queue until its connection attempt times out (several seconds), so leave this off if several trusted devices are usually off.

Before v5.4.0 Noctalia always ran this connection pass. It is now opt-in because on systems with many trusted devices that are usually off, the queued attempts kept the controller busy and delayed devices that reconnect on their own.

## Connection notifications

Set `connection_notifications = true` to show a notification when a Bluetooth device connects or disconnects without you acting on it in Noctalia, for example a headset you switch on, a device going out of range, or the connection pass above. Connecting, disconnecting, pairing, or forgetting a device from the Bluetooth tab stays silent, except that a connect you start which fails shows an error. Turning Bluetooth off does not notify for each device it drops, and devices that were already connected when Noctalia started never trigger one.
