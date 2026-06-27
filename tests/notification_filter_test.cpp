#include "config/config_types.h"
#include "notification/notification_filter.h"

#include <iostream>
#include <string>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

} // namespace

int main() {
  bool ok = true;

  const std::vector<std::string> blacklist = normalizeNotificationBlacklist({" Discord ", "discord", ""});

  ok &= check(blacklist.size() == 1, "blacklist dedupes and normalizes");
  ok &= check(
      notificationMatchesBlacklist(
          blacklist, NotificationFilterFields{.appName = "Discord", .category = std::nullopt, .desktopEntry = std::nullopt}
      ),
      "app name exact match"
  );
  ok &= check(
      notificationMatchesBlacklist(
          blacklist,
          NotificationFilterFields{
              .appName = "My Discord Client",
              .category = std::nullopt,
              .desktopEntry = std::nullopt,
          }
      ),
      "app name substring match"
  );
  ok &= check(
      !notificationMatchesBlacklist(
          blacklist,
          NotificationFilterFields{
              .appName = "Other",
              .category = std::nullopt,
              .desktopEntry = std::optional<std::string_view>{"org.telegram.desktop"},
          }
      ),
      "desktop entry no match"
  );
  ok &= check(
      notificationMatchesBlacklist(
          {"org.telegram.desktop"},
          NotificationFilterFields{
              .appName = "Telegram",
              .category = std::nullopt,
              .desktopEntry = std::optional<std::string_view>{"org.telegram.desktop"},
          }
      ),
      "desktop entry exact match"
  );
  ok &= check(
      notificationMatchesBlacklist(
          {"im.received"},
          NotificationFilterFields{
              .appName = "Chat",
              .category = std::optional<std::string_view>{"im.received"},
              .desktopEntry = std::nullopt,
          }
      ),
      "category exact match"
  );

  const auto allUrgencies = normalizeAllowedUrgencies({});
  ok &= check(urgencyIsAllowed(allUrgencies, Urgency::Low), "empty allowed list permits low");
  ok &= check(urgencyIsAllowed(allUrgencies, Urgency::Critical), "empty allowed list permits critical");

  const auto normalOnly = normalizeAllowedUrgencies({"normal", "invalid"});
  ok &= check(urgencyIsAllowed(normalOnly, Urgency::Normal), "configured normal allowed");
  ok &= check(!urgencyIsAllowed(normalOnly, Urgency::Low), "configured normal excludes low");

  std::vector<NotificationFilterConfig> filters = {
      NotificationFilterConfig{
          .name = "discord",
          .enabled = true,
          .match = "discord",
          .showToast = false,
          .saveHistory = false,
          .playSound = false,
      },
      NotificationFilterConfig{
          .name = "music",
          .enabled = true,
          .match = "x-gnome.music",
          .showToast = true,
          .saveHistory = false,
          .playSound = true,
      },
  };
  normalizeNotificationFilterNames(filters);

  const auto blocked = resolveNotificationFilter(
      filters,
      NotificationFilterFields{
          .appName = "Discord",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  ok &= check(blocked.matched && !blocked.showToast && !blocked.saveHistory, "first filter blocks discord");

  const auto lowOnlyFilter = resolveNotificationFilter(
      {NotificationFilterConfig{
          .name = "chat",
          .enabled = true,
          .match = "chat",
          .showToast = true,
          .saveHistory = true,
          .playSound = true,
          .allowedUrgencies = {"low"},
      }},
      NotificationFilterFields{
          .appName = "Chat",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  ok &= check(lowOnlyFilter.matched && urgencyIsAllowed(lowOnlyFilter.allowedUrgencies, Urgency::Low), "filter low allowed");
  ok &= check(!urgencyIsAllowed(lowOnlyFilter.allowedUrgencies, Urgency::Normal), "filter normal blocked");

  const auto noPermanent = resolveNotificationFilter(
      {NotificationFilterConfig{
          .name = "browser",
          .enabled = true,
          .match = "browser",
          .allowPermanent = false,
      }},
      NotificationFilterFields{
          .appName = "Browser",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  ok &= check(noPermanent.matched && !noPermanent.allowPermanent, "filter disallows permanent");
  ok &= check(resolveNotificationFilter({}, NotificationFilterFields{.appName = "Browser"}).allowPermanent, "default allows permanent");

  const auto music = resolveNotificationFilter(
      filters,
      NotificationFilterFields{
          .appName = "Rhythmbox",
          .category = std::optional<std::string_view>{"x-gnome.music"},
          .desktopEntry = std::nullopt,
      }
  );
  ok &= check(music.matched && music.showToast && !music.saveHistory && music.playSound, "music filter toast only");

  const auto unmatched = resolveNotificationFilter(
      filters,
      NotificationFilterFields{
          .appName = "Other App",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  ok &= check(!unmatched.matched && unmatched.showToast && unmatched.saveHistory, "default allow when no match");

  return ok ? 0 : 1;
}
