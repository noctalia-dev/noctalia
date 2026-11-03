#pragma once

#include <string>

struct Notification;

// Best-effort human-readable app label for notification UI (toast, history, control center).
std::string notificationDisplayAppName(const Notification& notification);
