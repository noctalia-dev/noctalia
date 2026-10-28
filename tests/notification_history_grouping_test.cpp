#include "notification/notification.h"
#include "shell/control_center/tabs/notifications_tab.h"
#include "tests/test_check.h"

#include <string>
#include <vector>

namespace {

  Notification
  makeNotification(uint32_t id, std::string appName, std::optional<std::string> desktopEntry = std::nullopt) {
    Notification n{};
    n.id = id;
    n.appName = std::move(appName);
    n.desktopEntry = std::move(desktopEntry);
    n.summary = "Test summary";
    n.body = "Test body";
    return n;
  }

  void testNormalizeAppKey() {
    TEST_CHECK(notification_grouping::normalizeAppKey("firefox.desktop") == "firefox");
    TEST_CHECK(notification_grouping::normalizeAppKey("/usr/share/applications/slack.desktop") == "slack");
    TEST_CHECK(notification_grouping::normalizeAppKey("  Discord  ") == "discord");
    TEST_CHECK(notification_grouping::normalizeAppKey("") == "unknown");
  }

  void testGroupKey() {
    const Notification n1 = makeNotification(1, "Slack", "slack.desktop");
    const Notification n2 = makeNotification(2, "slack", std::nullopt);
    const Notification n3 = makeNotification(3, "", "slack");

    TEST_CHECK(notification_grouping::groupKey(n1) == "slack");
    TEST_CHECK(notification_grouping::groupKey(n2) == "slack");
    TEST_CHECK(notification_grouping::groupKey(n3) == "slack");
  }

  void testGroupDisplayName() {
    const Notification n1 = makeNotification(1, "Slack", "slack.desktop");
    TEST_CHECK(notification_grouping::groupDisplayName(n1) == "Slack");

    const Notification n2 = makeNotification(2, "", "firefox");
    TEST_CHECK(!notification_grouping::groupDisplayName(n2).empty());
  }

  void testBuildVisualRowsAndStackInteractivity() {
    std::vector<AppNotificationGroup> groups;
    groups.push_back(
        AppNotificationGroup{
            .appKey = "slack",
            .appDisplayName = "Slack",
            .representativeId = 2,
            .notificationIds = {2, 1},
        }
    );
    groups.push_back(
        AppNotificationGroup{
            .appKey = "firefox",
            .appDisplayName = "Firefox",
            .representativeId = 3,
            .notificationIds = {3},
        }
    );

    // Default state: None expanded -> Slack is StackedCard, Firefox is single Notification
    std::unordered_set<std::string> expandedKeys;
    auto rows = notification_grouping::buildVisualRows(groups, expandedKeys);

    TEST_CHECK(rows.size() == 2);
    TEST_CHECK(rows[0].kind == HistoryRowItem::Kind::StackedCard);
    TEST_CHECK(rows[0].appKey == "slack");
    TEST_CHECK(rows[0].notificationId == 2);
    TEST_CHECK(rows[0].notificationCount == 2);
    TEST_CHECK(rows[0].collapsed);
    TEST_CHECK(!rows[0].hasAppHeader);
    TEST_CHECK(notification_grouping::isRowStackToggleInteractive(rows[0]));

    TEST_CHECK(rows[1].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[1].appKey == "firefox");
    TEST_CHECK(rows[1].notificationId == 3);
    TEST_CHECK(rows[1].notificationCount == 1);
    TEST_CHECK(!rows[1].isGroupParent);
    TEST_CHECK(!rows[1].hasAppHeader);
    TEST_CHECK(!notification_grouping::isRowStackToggleInteractive(rows[1]));

    // Expand Slack -> AppHeader + 2 notifications (first is group parent), Firefox remains single Notification
    expandedKeys.insert("slack");
    rows = notification_grouping::buildVisualRows(groups, expandedKeys);

    TEST_CHECK(rows.size() == 4);
    TEST_CHECK(rows[0].kind == HistoryRowItem::Kind::AppHeader);
    TEST_CHECK(rows[0].appKey == "slack");
    TEST_CHECK(rows[0].notificationCount == 2);
    TEST_CHECK(!rows[0].collapsed);
    TEST_CHECK(!rows[0].hasAppHeader);
    TEST_CHECK(notification_grouping::isRowStackToggleInteractive(rows[0]));

    TEST_CHECK(rows[1].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[1].notificationId == 2);
    TEST_CHECK(rows[1].isGroupParent);
    TEST_CHECK(rows[1].hasAppHeader);
    TEST_CHECK(notification_grouping::isRowStackToggleInteractive(rows[1]));

    TEST_CHECK(rows[2].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[2].notificationId == 1);
    TEST_CHECK(!rows[2].isGroupParent);
    TEST_CHECK(rows[2].hasAppHeader);
    TEST_CHECK(!notification_grouping::isRowStackToggleInteractive(rows[2]));

    TEST_CHECK(rows[3].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[3].notificationId == 3);
    TEST_CHECK(!rows[3].isGroupParent);
    TEST_CHECK(!rows[3].hasAppHeader);
    TEST_CHECK(!notification_grouping::isRowStackToggleInteractive(rows[3]));

    // Shrinking a group to 1 item or having stale keys for missing groups prunes them from expandedKeys
    expandedKeys.insert("firefox");
    expandedKeys.insert("phantom_app");
    rows = notification_grouping::buildVisualRows(groups, expandedKeys);
    TEST_CHECK(!expandedKeys.contains("firefox"));
    TEST_CHECK(!expandedKeys.contains("phantom_app"));

    // Disabling groupByApp emits flat Notification rows without headers or stack parent flags
    rows = notification_grouping::buildVisualRows(groups, expandedKeys, false);
    TEST_CHECK(rows.size() == 3);
    TEST_CHECK(rows[0].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[0].notificationId == 2);
    TEST_CHECK(!rows[0].isGroupParent);
    TEST_CHECK(!rows[0].hasAppHeader);
    TEST_CHECK(!notification_grouping::isRowStackToggleInteractive(rows[0]));
    TEST_CHECK(rows[1].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[1].notificationId == 1);
    TEST_CHECK(!rows[1].isGroupParent);
    TEST_CHECK(!rows[1].hasAppHeader);
    TEST_CHECK(rows[2].kind == HistoryRowItem::Kind::Notification);
    TEST_CHECK(rows[2].notificationId == 3);
    TEST_CHECK(!rows[2].isGroupParent);
    TEST_CHECK(!rows[2].hasAppHeader);
    TEST_CHECK(expandedKeys.empty());
  }

  void testPruneGroupsUpdatesRepresentativeAndErasesEmpty() {
    std::vector<AppNotificationGroup> groups;
    groups.push_back(
        AppNotificationGroup{
            .appKey = "slack",
            .appDisplayName = "Slack",
            .representativeId = 5,
            .notificationIds = {5, 4, 3},
        }
    );
    groups.push_back(
        AppNotificationGroup{
            .appKey = "firefox",
            .appDisplayName = "Firefox",
            .representativeId = 9,
            .notificationIds = {9},
        }
    );
    groups.push_back(
        AppNotificationGroup{
            .appKey = "mail",
            .appDisplayName = "Mail",
            .representativeId = 7,
            .notificationIds = {7, 6},
        }
    );

    // Nothing vanished: no change, representatives untouched.
    TEST_CHECK(!notification_grouping::pruneGroups(groups, [](uint32_t) { return false; }));
    TEST_CHECK(groups.size() == 3);
    TEST_CHECK(groups[0].representativeId == 5);

    // Newest Slack entry (5) and the only Firefox entry (9) vanish; Mail is untouched.
    // Pruning with expandedKeys should remove empty groups (firefox) and preserve multi-item groups (slack, mail).
    std::unordered_set<std::string> expandedKeys = {"slack", "firefox", "mail"};
    const auto vanished = [](uint32_t id) { return id == 5 || id == 9; };
    TEST_CHECK(notification_grouping::pruneGroups(groups, vanished, &expandedKeys));

    TEST_CHECK(groups.size() == 2);
    TEST_CHECK(groups[0].appKey == "slack");
    TEST_CHECK(groups[0].notificationIds == std::vector<uint32_t>({4, 3}));
    TEST_CHECK(groups[0].representativeId == 4);
    TEST_CHECK(groups[1].appKey == "mail");
    TEST_CHECK(groups[1].representativeId == 7);
    TEST_CHECK(expandedKeys.contains("slack"));
    TEST_CHECK(!expandedKeys.contains("firefox"));
    TEST_CHECK(expandedKeys.contains("mail"));

    // When mail shrinks to 1 item, it should also be pruned from expandedKeys.
    const auto mailVanished = [](uint32_t id) { return id == 7; };
    TEST_CHECK(notification_grouping::pruneGroups(groups, mailVanished, &expandedKeys));
    TEST_CHECK(groups.size() == 2);
    TEST_CHECK(groups[1].notificationIds.size() == 1);
    TEST_CHECK(!expandedKeys.contains("mail"));
  }

} // namespace

int main() {
  testNormalizeAppKey();
  testGroupKey();
  testGroupDisplayName();
  testBuildVisualRowsAndStackInteractivity();
  testPruneGroupsUpdatesRepresentativeAndErasesEmpty();
  return 0;
}
