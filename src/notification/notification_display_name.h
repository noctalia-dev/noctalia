#pragma once

#include "system/desktop_entry.h"

#include <optional>
#include <string>
#include <string_view>

struct Notification;

// Best-effort human-readable app label for notification UI (toast, history, control center).
std::string notificationDisplayAppName(const Notification& notification);

// Resolves a notification `desktop-entry` hint (optionally with a `.desktop` suffix or path) to its entry.
std::optional<DesktopEntry> findNotificationDesktopEntry(std::string_view key);
