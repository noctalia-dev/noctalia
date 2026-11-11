#include "config/config_types.h"
#include "notification/notification_filter.h"
#include "tests/test_check.h"

#include <string>

int main() {
  const std::vector<std::string> blacklist = normalizeNotificationBlacklist({" Discord ", "discord", ""});

  TEST_CHECK(blacklist.size() == 1);
  TEST_CHECK(notificationMatchesBlacklist(
      blacklist, NotificationFilterFields{.appName = "Discord", .category = std::nullopt, .desktopEntry = std::nullopt}
  ));
  TEST_CHECK(notificationMatchesBlacklist(
      blacklist,
      NotificationFilterFields{
          .appName = "My Discord Client",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  ));
  TEST_CHECK(!notificationMatchesBlacklist(
      blacklist,
      NotificationFilterFields{
          .appName = "Other",
          .category = std::nullopt,
          .desktopEntry = std::optional<std::string_view>{"org.telegram.desktop"},
      }
  ));
  TEST_CHECK(notificationMatchesBlacklist(
      {"org.telegram.desktop"},
      NotificationFilterFields{
          .appName = "Telegram",
          .category = std::nullopt,
          .desktopEntry = std::optional<std::string_view>{"org.telegram.desktop"},
      }
  ));
  TEST_CHECK(notificationMatchesBlacklist(
      {"im.received"},
      NotificationFilterFields{
          .appName = "Chat",
          .category = std::optional<std::string_view>{"im.received"},
          .desktopEntry = std::nullopt,
      }
  ));

  const auto allUrgencies = normalizeAllowedUrgencies({});
  TEST_CHECK(urgencyIsAllowed(allUrgencies, Urgency::Low));
  TEST_CHECK(urgencyIsAllowed(allUrgencies, Urgency::Critical));

  const auto normalOnly = normalizeAllowedUrgencies({"normal", "invalid"});
  TEST_CHECK(urgencyIsAllowed(normalOnly, Urgency::Normal));
  TEST_CHECK(!urgencyIsAllowed(normalOnly, Urgency::Low));

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
  TEST_CHECK(blocked.matched && !blocked.showToast && !blocked.saveHistory);

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
  TEST_CHECK(lowOnlyFilter.matched && urgencyIsAllowed(lowOnlyFilter.allowedUrgencies, Urgency::Low));
  TEST_CHECK(!urgencyIsAllowed(lowOnlyFilter.allowedUrgencies, Urgency::Normal));

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
  TEST_CHECK(noPermanent.matched && !noPermanent.allowPermanent);
  const auto dndBypass = resolveNotificationFilter(
      {NotificationFilterConfig{
          .name = "medication",
          .enabled = true,
          .match = "medication",
          .bypassDnd = true,
      }},
      NotificationFilterFields{
          .appName = "Medication Reminder",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  TEST_CHECK(dndBypass.matched && dndBypass.bypassDnd);
  TEST_CHECK(!resolveNotificationFilter({}, NotificationFilterFields{.appName = "Medication Reminder"}).bypassDnd);
  TEST_CHECK(resolveNotificationFilter({}, NotificationFilterFields{.appName = "Browser"}).allowPermanent);

  const auto music = resolveNotificationFilter(
      filters,
      NotificationFilterFields{
          .appName = "Rhythmbox",
          .category = std::optional<std::string_view>{"x-gnome.music"},
          .desktopEntry = std::nullopt,
      }
  );
  TEST_CHECK(music.matched && music.showToast && !music.saveHistory && music.playSound);

  const auto unmatched = resolveNotificationFilter(
      filters,
      NotificationFilterFields{
          .appName = "Other App",
          .category = std::nullopt,
          .desktopEntry = std::nullopt,
      }
  );
  TEST_CHECK(!unmatched.matched && unmatched.showToast && unmatched.saveHistory);

  return 0;
}
