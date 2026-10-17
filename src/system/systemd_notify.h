#pragma once

/** Notify systemd that the application is ready.
 *  Returns true if
 *  - The notification was sent successfully
 *  - The NOTIFY_SOCKET environment variable is not set, and the notification is therefore skipped, when
 *    - Noctalia is not started as a systemd service
 *    - Noctalia is started as a systemd service but not in `Type=notify` mode.
 *  Returns false if NOTIFY_SOCKET environment variable is set but the notification could not be sent.
 */
[[nodiscard]] bool systemdNotifyReady(void);
