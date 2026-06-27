#include "notification/notification_display_name.h"

#include "notification/notification.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/internal_app_metadata.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace {

  constexpr app_identity::DesktopEntryLookupOptions kLookupOptions{
      .includeHidden = true,
      .includeNoDisplay = true,
  };

  std::string normalizeDesktopKey(std::string_view key) {
    std::string normalized(key);
    if (normalized.ends_with(".desktop")) {
      normalized.erase(normalized.size() - std::string_view(".desktop").size());
    }
    const auto lastSlash = normalized.find_last_of('/');
    if (lastSlash != std::string::npos) {
      normalized = normalized.substr(lastSlash + 1);
    }
    return normalized;
  }

  std::optional<std::string> nameFromDesktopKey(std::string_view key) {
    if (key.empty()) {
      return std::nullopt;
    }
    const auto entry = app_identity::findDesktopEntry(normalizeDesktopKey(key), desktopEntries(), kLookupOptions);
    if (!entry.has_value()) {
      return std::nullopt;
    }
    if (!entry->name.empty()) {
      return entry->name;
    }
    if (!entry->genericName.empty()) {
      return entry->genericName;
    }
    return std::nullopt;
  }

  std::string prettifyIdentifier(std::string value) {
    if (value.empty()) {
      return value;
    }
    for (char& ch : value) {
      if (ch == '-' || ch == '_' || ch == '.') {
        ch = ' ';
      }
    }
    bool capitalize = true;
    for (char& ch : value) {
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
        capitalize = true;
        continue;
      }
      if (capitalize) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        capitalize = false;
      }
    }
    return value;
  }

  [[nodiscard]] bool looksLikeTechnicalId(std::string_view name) {
    if (name.empty() || name.contains(' ')) {
      return false;
    }
    if (name.contains('.')) {
      return true;
    }
    bool hasUpper = false;
    bool hasLower = false;
    for (const char ch : name) {
      if (std::isupper(static_cast<unsigned char>(ch)) != 0) {
        hasUpper = true;
      }
      if (std::islower(static_cast<unsigned char>(ch)) != 0) {
        hasLower = true;
      }
    }
    if (!hasUpper && hasLower) {
      return true;
    }
    return name.contains('-') || name.contains('_');
  }

  std::string resolveFromKeys(std::string_view primary, const std::optional<std::string>& desktopEntry) {
    if (desktopEntry.has_value()) {
      if (auto name = nameFromDesktopKey(*desktopEntry)) {
        return *name;
      }
    }
    if (auto name = nameFromDesktopKey(primary)) {
      return *name;
    }
    if (const auto dot = primary.rfind('.'); dot != std::string_view::npos && dot + 1 < primary.size()) {
      if (auto name = nameFromDesktopKey(primary.substr(dot + 1))) {
        return *name;
      }
    }
    if (const auto meta = internal_apps::metadataForAppId(std::string(primary)); meta.has_value()) {
      return meta->displayName;
    }
    if (looksLikeTechnicalId(primary)) {
      return prettifyIdentifier(std::string(primary));
    }
    return std::string(primary);
  }

} // namespace

std::string notificationDisplayAppName(const Notification& notification) {
  if (notification.appName.empty()) {
    return notification.appName;
  }
  return resolveFromKeys(notification.appName, notification.desktopEntry);
}
