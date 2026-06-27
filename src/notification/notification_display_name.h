#pragma once

#include <string>

struct Notification;

// Best-effort human-readable app label for notification UI (toast, history, control center).
[[nodiscard]] std::string notificationDisplayAppName(const Notification& notification);
