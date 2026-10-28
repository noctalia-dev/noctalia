#include "shell/control_center/tabs/notifications_tab.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/notification.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/animation/animation.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace control_center;

namespace notification_grouping {

  std::string normalizeAppKey(std::string_view key) {
    std::string normalized(key);
    if (normalized.ends_with(".desktop")) {
      normalized.erase(normalized.size() - std::string_view(".desktop").size());
    }
    const auto lastSlash = normalized.find_last_of('/');
    if (lastSlash != std::string::npos) {
      normalized = normalized.substr(lastSlash + 1);
    }
    normalized = StringUtils::toLower(StringUtils::trim(normalized));
    return normalized.empty() ? "unknown" : normalized;
  }

  std::string groupKey(const Notification& notification) {
    const std::string displayName = notificationDisplayAppName(notification);
    if (!displayName.empty()) {
      return StringUtils::toLower(StringUtils::trim(displayName));
    }
    if (notification.desktopEntry.has_value() && !notification.desktopEntry->empty()) {
      return normalizeAppKey(*notification.desktopEntry);
    }
    if (!notification.appName.empty()) {
      return normalizeAppKey(notification.appName);
    }
    return "unknown";
  }

  std::string groupDisplayName(const Notification& notification) {
    const std::string name = notificationDisplayAppName(notification);
    if (!name.empty()) {
      return name;
    }
    if (notification.desktopEntry.has_value() && !notification.desktopEntry->empty()) {
      return normalizeAppKey(*notification.desktopEntry);
    }
    if (!notification.appName.empty()) {
      return notification.appName;
    }
    return i18n::tr("control-center.notifications.untitled");
  }

  bool pruneGroups(
      std::vector<AppNotificationGroup>& groups, const std::function<bool(uint32_t)>& isVanished,
      std::unordered_set<std::string>* expandedAppKeys
  ) {
    bool changed = false;
    for (auto it = groups.begin(); it != groups.end();) {
      const bool groupChanged = std::erase_if(it->notificationIds, isVanished) > 0;
      changed = changed || groupChanged;
      if (it->notificationIds.empty()) {
        if (expandedAppKeys != nullptr) {
          expandedAppKeys->erase(it->appKey);
        }
        it = groups.erase(it);
        continue;
      }
      if (it->notificationIds.size() == 1 && expandedAppKeys != nullptr) {
        expandedAppKeys->erase(it->appKey);
      }
      if (groupChanged) {
        it->representativeId = it->notificationIds.front();
      }
      ++it;
    }
    return changed;
  }

  namespace {

    void appendSingleNotificationRow(std::vector<HistoryRowItem>& rows, const AppNotificationGroup& group) {
      rows.push_back(
          HistoryRowItem{
              .kind = HistoryRowItem::Kind::Notification,
              .appKey = group.appKey,
              .appDisplayName = group.appDisplayName,
              .notificationId = group.notificationIds.front(),
              .representativeId = group.representativeId,
              .notificationCount = 1,
              .collapsed = false,
              .isGroupParent = false,
              .hasAppHeader = false,
          }
      );
    }

    void appendCollapsedStackRow(std::vector<HistoryRowItem>& rows, const AppNotificationGroup& group) {
      rows.push_back(
          HistoryRowItem{
              .kind = HistoryRowItem::Kind::StackedCard,
              .appKey = group.appKey,
              .appDisplayName = group.appDisplayName,
              .notificationId = group.notificationIds.front(),
              .representativeId = group.representativeId,
              .notificationCount = group.notificationIds.size(),
              .collapsed = true,
              .isGroupParent = false,
              .hasAppHeader = false,
          }
      );
    }

    void appendExpandedStackRows(std::vector<HistoryRowItem>& rows, const AppNotificationGroup& group) {
      rows.push_back(
          HistoryRowItem{
              .kind = HistoryRowItem::Kind::AppHeader,
              .appKey = group.appKey,
              .appDisplayName = group.appDisplayName,
              .representativeId = group.representativeId,
              .notificationCount = group.notificationIds.size(),
              .collapsed = false,
              .isGroupParent = false,
              .hasAppHeader = false,
          }
      );
      for (std::size_t i = 0; i < group.notificationIds.size(); ++i) {
        const uint32_t id = group.notificationIds[i];
        rows.push_back(
            HistoryRowItem{
                .kind = HistoryRowItem::Kind::Notification,
                .appKey = group.appKey,
                .appDisplayName = group.appDisplayName,
                .notificationId = id,
                .representativeId = group.representativeId,
                .notificationCount = group.notificationIds.size(),
                .collapsed = false,
                .isGroupParent = (i == 0),
                .hasAppHeader = true,
            }
        );
      }
    }

  } // namespace

  std::vector<HistoryRowItem> buildVisualRows(
      const std::vector<AppNotificationGroup>& groups, std::unordered_set<std::string>& expandedAppKeys, bool groupByApp
  ) {
    std::unordered_set<std::string> liveMultiItemKeys;
    if (groupByApp) {
      for (const auto& group : groups) {
        if (group.notificationIds.size() > 1) {
          liveMultiItemKeys.insert(group.appKey);
        }
      }
    }
    std::erase_if(expandedAppKeys, [&](const std::string& key) { return !liveMultiItemKeys.contains(key); });

    std::vector<HistoryRowItem> rows;
    if (!groupByApp) {
      for (const auto& group : groups) {
        for (const uint32_t id : group.notificationIds) {
          rows.push_back(
              HistoryRowItem{
                  .kind = HistoryRowItem::Kind::Notification,
                  .appKey = group.appKey,
                  .appDisplayName = group.appDisplayName,
                  .notificationId = id,
                  .representativeId = id,
                  .notificationCount = 1,
                  .collapsed = false,
                  .isGroupParent = false,
                  .hasAppHeader = false,
              }
          );
        }
      }
      return rows;
    }

    for (const auto& group : groups) {
      if (group.notificationIds.empty()) {
        continue;
      }

      if (group.notificationIds.size() == 1) {
        appendSingleNotificationRow(rows, group);
      } else if (!expandedAppKeys.contains(group.appKey)) {
        appendCollapsedStackRow(rows, group);
      } else {
        appendExpandedStackRows(rows, group);
      }
    }
    return rows;
  }

  bool isRowStackToggleInteractive(const HistoryRowItem& row) noexcept {
    return row.kind != HistoryRowItem::Kind::Notification || row.isGroupParent;
  }

} // namespace notification_grouping

namespace {

  constexpr Logger kLog("control-center-notifications");

  constexpr float kHistoryIconSize = 36.0F;
  constexpr float kHistoryIconGlyphSize = 22.0F;
  constexpr float kHistoryIconReferenceSize = 36.0F;
  constexpr float kAppHeaderIconSize = 18.0F;
  constexpr float kAppHeaderIconGlyphSize = 12.0F;
  constexpr float kAppHeaderActionButtonSize = 22.0F;
  constexpr float kAppHeaderActionGlyphSize = 12.0F;
  constexpr float kNotificationActionButtonSize = Style::controlHeightSm;
  constexpr float kStackShelf1Offset = 6.0F;
  constexpr float kStackShelf1Inset = 8.0F;
  constexpr float kStackShelf2Offset = 11.0F;
  constexpr float kStackShelf2Inset = 16.0F;
  constexpr float kStackShelfHeight = 32.0F;

  float measureAppHeader(float scale) {
    const float paddingY = Style::spaceXs * scale;
    const float contentH = std::max(kAppHeaderIconSize * scale, kAppHeaderActionButtonSize * scale);
    return paddingY + contentH;
  }

  float notificationIconRadius(float iconSize, float localScale) {
    const float baseRadius = Style::radiusMd * (iconSize / kHistoryIconReferenceSize);
    return std::min(iconSize * 0.5F, Style::scaledRadius(baseRadius, localScale));
  }

  std::string historyActionLabel(std::string_view actionKey, std::string_view actionLabel) {
    if (!StringUtils::isBlank(actionLabel)) {
      return std::string(actionLabel);
    }
    if (actionKey == "default") {
      return i18n::tr("notifications.actions.open");
    }
    if (actionKey == "inline-reply") {
      return i18n::tr("notifications.inline-reply.button");
    }
    return i18n::tr("notifications.actions.fallback");
  }

  float measureHistoryActionsRowHeight(
      Renderer& renderer, const std::vector<std::string>& actions, float cardTextWidth, float scale
  ) {
    if (actions.empty()) {
      return 0.0F;
    }
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });

    std::vector<std::unique_ptr<Button>> buttons;
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      const std::string& actionKey = actions[i];
      if (actionKey.empty()) {
        continue;
      }
      buttons.push_back(
          ui::button({
              .text = historyActionLabel(actionKey, actions[i + 1]),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Default,
          })
      );
    }

    auto rows = wrapButtonsIntoRows(renderer, buttons, cardTextWidth, Style::spaceXs * scale);
    populateRowContainer(*container, std::move(rows), cardTextWidth, Style::spaceXs * scale);

    if (container->children().empty()) {
      return 0.0F;
    }

    container->setSize(cardTextWidth, 0.0F);
    container->layout(renderer);
    return container->height();
  }
  constexpr int kSummaryMaxLines = 2;
  constexpr int kBodyMaxLines = 3;
  constexpr int kExpandedMaxLines = 500;

  std::filesystem::path remoteNotificationIconCachePath(std::string_view url) {
    return std::filesystem::path("/tmp")
        / "noctalia-notification-icons"
        / (std::to_string(std::hash<std::string_view>{}(url)) + ".img");
  }

  std::string normalizeLocalIconPath(std::string_view iconValue) { return uri::normalizeFileUrl(iconValue); }

  std::string resolveHistoryIconPath(const Notification& n, IconResolver& resolver, int targetSize) {
    if (!n.icon.has_value() || n.icon->empty()) {
      return {};
    }
    const std::string& iconValue = *n.icon;
    if (uri::isRemoteUrl(iconValue)) {
      const auto cached = remoteNotificationIconCachePath(iconValue);
      std::error_code ec;
      if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
        return cached.string();
      }
      return {};
    }

    std::string localPath = normalizeLocalIconPath(iconValue);
    if (!localPath.empty() && localPath.front() == '/') {
      if (access(localPath.c_str(), R_OK) == 0) {
        return localPath;
      }
      return {};
    }
    if (localPath.empty()) {
      return {};
    }

    const std::string& resolved = resolver.resolve(localPath, targetSize);
    return resolved.empty() ? std::string() : resolved;
  }

  void applyNotificationCardStyle(Flex& card, float scale, float fillOpacity) {
    applySectionCardStyle(card, scale, fillOpacity);
  }

  std::string relativeMetaLine(const Notification& n) {
    if (n.receivedWallClock.has_value()) {
      return formatTimeAgo(*n.receivedWallClock);
    }
    return formatElapsedSince(n.receivedTime);
  }

  std::int64_t currentRelativeTimeSlot() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
        / 15;
  }

  bool matchesHistoryFilter(const NotificationHistoryEntry& e, std::size_t filterIndex) {
    if (filterIndex == 0) {
      return true;
    }
    if (!e.notification.receivedWallClock.has_value()) {
      return false;
    }
    const std::time_t entryT = WallClock::to_time_t(*e.notification.receivedWallClock);
    const std::time_t nowT = WallClock::to_time_t(WallClock::now());
    std::tm entryL{};
    std::tm nowL{};
    localtime_r(&entryT, &entryL);
    localtime_r(&nowT, &nowL);
    const bool isToday = entryL.tm_year == nowL.tm_year && entryL.tm_yday == nowL.tm_yday;
    std::tm yRef = nowL;
    yRef.tm_hour = 12;
    yRef.tm_min = 0;
    yRef.tm_sec = 0;
    yRef.tm_mday -= 1;
    mktime(&yRef);
    const bool isYesterday = entryL.tm_year == yRef.tm_year && entryL.tm_yday == yRef.tm_yday;

    if (filterIndex == 1) {
      return isToday;
    }
    if (filterIndex == 2) {
      return isYesterday;
    }
    return !isToday && !isYesterday;
  }

  float measuredTextHeight(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines
  ) {
    if (text.empty()) {
      return 0.0F;
    }
    const auto bounds = renderer.measureText(text, fontSize, {fontWeight}, maxWidth, maxLines);
    return std::max(0.0F, bounds.bottom - bounds.top);
  }

  bool canExpandText(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth,
      int collapsedMaxLines
  ) {
    if (text.empty()) {
      return false;
    }

    const float collapsedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, collapsedMaxLines);
    const float expandedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, kExpandedMaxLines);
    return expandedHeight > collapsedHeight + 0.5F;
  }

  struct NotificationCardMetrics {
    std::string summaryText;
    std::string bodyText;
    std::string metaLine;
    bool canExpand = false;
    bool expanded = false;
    float height = 0.0F;
    float cardTextWidth = 0.0F;
    float metaTextWidth = 0.0F;
  };

  NotificationCardMetrics measureNotificationCard(
      Renderer& renderer, const NotificationHistoryEntry& entry, float scale, float width, bool expandedRequested,
      bool showHistoryActions, bool isStackedCard = false, bool showAppName = true
  ) {
    NotificationCardMetrics metrics;
    const float cardWidth = std::max(0.0F, width);
    const float cardHorizontalPadding = Style::spaceMd * scale * 2.0F;
    metrics.cardTextWidth = std::max(0.0F, cardWidth - cardHorizontalPadding);
    const std::string summaryText = StringUtils::trimLeadingBlankLines(
        entry.notification.summary.empty() ? i18n::tr("control-center.notifications.untitled")
                                           : entry.notification.summary
    );
    const std::string bodyText = StringUtils::trimLeadingBlankLines(entry.notification.body);
    metrics.summaryText = summaryText;

    if (isStackedCard) {
      metrics.canExpand = true;
      metrics.expanded = false;
      bool bodyLineTruncated = false;
      metrics.bodyText = StringUtils::truncateByLines(bodyText, kBodyMaxLines, &bodyLineTruncated);
    } else {
      const bool summaryExpandable = canExpandText(
          renderer, summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth, kSummaryMaxLines
      );
      bool bodyLineTruncated = false;
      const std::string collapsedBodyText = StringUtils::truncateByLines(bodyText, kBodyMaxLines, &bodyLineTruncated);
      const bool bodyExpandable = bodyLineTruncated
          || canExpandText(renderer, bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
                           kBodyMaxLines);
      metrics.canExpand = summaryExpandable || bodyExpandable;
      metrics.expanded = metrics.canExpand && expandedRequested;
      metrics.bodyText = metrics.expanded ? bodyText : collapsedBodyText;
    }

    const float iconPx = kHistoryIconSize * scale;
    const float iconColumn = iconPx + Style::spaceSm * scale;
    const float actionButtonSize = kNotificationActionButtonSize * scale;
    const float actionButtonsGap = Style::spaceXs * scale;
    const float headerActionsWidth = isStackedCard
        ? (actionButtonSize * 2.0F + actionButtonsGap)
        : (actionButtonSize + (metrics.canExpand ? (actionButtonsGap + actionButtonSize) : 0.0F));
    const float leftClusterWidth = metrics.cardTextWidth - headerActionsWidth;
    constexpr float kStackBadgeEstimatedWidth = 24.0F;
    const float badgeExtra = isStackedCard ? ((kStackBadgeEstimatedWidth + Style::spaceSm) * scale) : 0.0F;
    metrics.metaTextWidth = std::max(0.0F, leftClusterWidth - iconColumn - badgeExtra);

    const std::string relativeTime = relativeMetaLine(entry.notification);
    if (showAppName) {
      const std::string appName = notificationDisplayAppName(entry.notification);
      metrics.metaLine = appName.empty() ? relativeTime : appName + " • " + relativeTime;
    } else {
      metrics.metaLine = relativeTime;
    }

    const float metaHeight = measuredTextHeight(
        renderer, metrics.metaLine, Style::fontSizeMini * scale, FontWeight::Normal, metrics.metaTextWidth, 0
    );
    const float headerHeight = std::max({iconPx, actionButtonSize, metaHeight});
    const float summaryHeight = measuredTextHeight(
        renderer, metrics.summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth,
        metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines
    );
    const float bodyHeight = metrics.bodyText.empty()
        ? 0.0F
        : measuredTextHeight(
              renderer, metrics.bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
              metrics.expanded ? kExpandedMaxLines : kBodyMaxLines
          );

    const float actionsRowHeight = showHistoryActions
        ? measureHistoryActionsRowHeight(renderer, entry.notification.actions, metrics.cardTextWidth, scale)
        : 0.0F;

    const float paddingY = (Style::spaceSm + Style::spaceXs) * scale * 2.0F;
    int visibleSegments = 2;
    if (!metrics.bodyText.empty()) {
      ++visibleSegments;
    }
    if (actionsRowHeight > 0.5F) {
      ++visibleSegments;
    }
    const float gaps = Style::spaceSm * scale * static_cast<float>(std::max(0, visibleSegments - 1));
    metrics.height = paddingY + headerHeight + summaryHeight + bodyHeight + actionsRowHeight + gaps;
    return metrics;
  }

  std::uint64_t rawImageKey(const NotificationHistoryEntry& entry) {
    return (static_cast<std::uint64_t>(entry.notification.id) << 32U) ^ entry.eventSerial;
  }

  std::uint64_t revisionForEntry(const NotificationHistoryEntry& entry, bool expanded, std::int64_t relativeSlot) {
    std::uint64_t revision = entry.eventSerial;
    revision ^= static_cast<std::uint64_t>(relativeSlot < 0 ? 0 : relativeSlot) * 0x9E3779B185EBCA87ULL;
    if (expanded) {
      revision ^= 0xD1B54A32D192ED03ULL;
    }
    return revision;
  }

  class NotificationHistoryRow final : public Flex {
  private:
    void initStackShelves(float scale, float fillOpacity) {
      m_shelf2 = static_cast<Box*>(addChild(
          ui::box({
              .visible = false,
              .participatesInLayout = false,
          })
      ));
      m_shelf2->setCardStyle(scale, fillOpacity * 0.84F);

      m_shelf1 = static_cast<Box*>(addChild(
          ui::box({
              .visible = false,
              .participatesInLayout = false,
          })
      ));
      m_shelf1->setCardStyle(scale, fillOpacity * 0.92F);
    }

    void initAppHeaderContainer(float scale) {
      m_headerContainer = static_cast<Flex*>(addChild(
          ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm * scale,
              .fillWidth = true,
              .visible = false,
          })
      ));
      m_headerContainer->setDirection(FlexDirection::Horizontal);
      m_headerContainer->setAlign(FlexAlign::Center);
      m_headerContainer->setJustify(FlexJustify::SpaceBetween);
      m_headerContainer->setPadding(Style::spaceXs * 0.5F * scale, Style::spaceSm * scale);
      m_headerContainer->clearBorder();
      m_headerContainer->setFill(clearColorSpec());

      m_headerLeftCluster = static_cast<Flex*>(m_headerContainer->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_headerIconSlot = static_cast<Box*>(m_headerLeftCluster->addChild(
          ui::box({
              .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
              .radius = notificationIconRadius(kAppHeaderIconSize, scale),
              .width = kAppHeaderIconSize * scale,
              .height = kAppHeaderIconSize * scale,
          })
      ));

      m_headerImage = static_cast<Image*>(m_headerIconSlot->addChild(
          ui::image({
              .visible = false,
          })
      ));

      m_headerFallback = static_cast<Glyph*>(m_headerIconSlot->addChild(
          ui::glyph({
              .glyph = "bell",
              .visible = false,
          })
      ));

      m_headerTitle = static_cast<Label*>(m_headerLeftCluster->addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
          })
      ));

      m_headerBadge = static_cast<Flex*>(m_headerLeftCluster->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .paddingH = Style::spaceXs * 0.75F * scale,
              .fill = colorSpecFromRole(ColorRole::SecondaryContainer),
              .radius = Style::scaledRadiusSm(scale),
          })
      ));

      m_headerBadgeLabel = static_cast<Label*>(m_headerBadge->addChild(
          ui::label({
              .fontSize = Style::fontSizeMini * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSecondaryContainer),
          })
      ));

      m_headerRightActions = static_cast<Flex*>(m_headerContainer->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
          })
      ));

      m_headerClear = static_cast<Button*>(m_headerRightActions->addChild(makeHeaderActionButton("trash", scale)));
      m_headerChevron =
          static_cast<Button*>(m_headerRightActions->addChild(makeHeaderActionButton("chevron-up", scale)));
    }

    void initNotificationContainer(float scale, float fillOpacity) {
      m_notificationContainer = static_cast<Flex*>(addChild(
          ui::column({
              .align = FlexAlign::Stretch,
              .fillWidth = true,
              .visible = false,
          })
      ));
      applyNotificationCardStyle(*m_notificationContainer, scale, fillOpacity);

      m_header = static_cast<Flex*>(m_notificationContainer->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm * scale,
          })
      ));

      m_leftCluster = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_iconSlot = static_cast<Box*>(m_leftCluster->addChild(
          ui::box({
              .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
              .radius = notificationIconRadius(kHistoryIconSize, scale),
              .width = kHistoryIconSize * scale,
              .height = kHistoryIconSize * scale,
          })
      ));

      m_image = static_cast<Image*>(m_iconSlot->addChild(
          ui::image({
              .visible = false,
          })
      ));

      m_fallback = static_cast<Glyph*>(m_iconSlot->addChild(
          ui::glyph({
              .glyph = "bell",
              .visible = false,
          })
      ));

      m_meta = static_cast<Label*>(m_leftCluster->addChild(
          ui::label({
              .fontSize = Style::fontSizeMini * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_stackBadge = static_cast<Flex*>(m_leftCluster->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .paddingH = Style::spaceXs * scale,
              .fill = colorSpecFromRole(ColorRole::SecondaryContainer),
              .radius = Style::scaledRadiusSm(scale),
              .visible = false,
          })
      ));
      m_stackBadge->setParticipatesInLayout(false);

      m_stackBadgeLabel = static_cast<Label*>(m_stackBadge->addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSecondaryContainer),
          })
      ));

      m_headerActions = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
          })
      ));

      m_expand = static_cast<Button*>(m_headerActions->addChild(makeActionButton("chevron-down", scale)));
      m_dismiss = static_cast<Button*>(m_headerActions->addChild(makeActionButton("trash", scale)));

      m_summary = static_cast<Label*>(m_notificationContainer->addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
          })
      ));

      m_body = static_cast<Label*>(m_notificationContainer->addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      ));

      m_actionsRow = static_cast<Flex*>(m_notificationContainer->addChild(
          ui::column({
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * scale,
              .fillWidth = true,
              .visible = false,
          })
      ));
    }

  public:
    explicit NotificationHistoryRow(float scale, float fillOpacity) : m_scale(scale) {
      setFillWidth(true);
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setPadding(0.0F);
      setGap(0.0F);

      initStackShelves(scale, fillOpacity);
      initAppHeaderContainer(scale);
      initNotificationContainer(scale, fillOpacity);
    }

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      if (m_isStackedCard) {
        applyShelfLayout();
      }
    }

  public:
    void applyShelfLayout() {
      const float w = width();
      const float cardH = m_notificationContainer != nullptr ? m_notificationContainer->height() : 0.0F;
      const float shelfH = kStackShelfHeight * m_scale;

      const float s1Inset = kStackShelf1Inset * m_scale;
      const float s1Offset = kStackShelf1Offset * m_scale;
      const float s1W = std::max(0.0F, w - s1Inset * 2.0F);
      const float s1Y = cardH + s1Offset - shelfH;
      m_shelf1->setPosition(s1Inset, s1Y);
      m_shelf1->setSize(s1W, shelfH);

      if (m_shelf2Visible) {
        const float s2Inset = kStackShelf2Inset * m_scale;
        const float s2Offset = kStackShelf2Offset * m_scale;
        const float s2W = std::max(0.0F, w - s2Inset * 2.0F);
        const float s2Y = cardH + s2Offset - shelfH;
        m_shelf2->setPosition(s2Inset, s2Y);
        m_shelf2->setSize(s2W, shelfH);
      }
    }

    void bindHeader(
        Renderer& renderer, const HistoryRowItem& row, const NotificationHistoryEntry* repEntry, float width,
        IconResolver& iconResolver, const std::function<void(const std::string&)>& onToggleExpanded,
        std::function<void(const std::string&)> onClear
    ) {
      if (m_imageKind != ImageKind::None) {
        m_image->clear(renderer);
        m_imageKind = ImageKind::None;
        m_rawImageKey = 0;
      }

      m_isStackedCard = false;
      m_shelf2Visible = false;
      m_shelf1->setVisible(false);
      m_shelf2->setVisible(false);
      m_stackBadge->setVisible(false);
      m_stackBadge->setParticipatesInLayout(false);

      m_headerContainer->setVisible(true);
      m_headerContainer->setParticipatesInLayout(true);
      m_notificationContainer->setVisible(false);
      m_notificationContainer->setParticipatesInLayout(false);

      const float height = measureAppHeader(m_scale);
      setSize(width, height);
      m_headerContainer->setSize(width, height);

      const float iconPx = kAppHeaderIconSize * m_scale;
      m_headerIconSlot->setSize(iconPx, iconPx);
      bindHeaderIcon(renderer, repEntry, iconResolver);

      m_headerTitle->setText(row.appDisplayName);

      m_headerBadgeLabel->setText(std::to_string(row.notificationCount));
      m_headerBadgeLabel->measure(renderer);

      const float actionButtonSize = kAppHeaderActionButtonSize * m_scale;
      const float actionButtonsGap = Style::spaceXs * m_scale;
      const float actionsWidth = actionButtonSize * 2.0F + actionButtonsGap;
      const float paddingH = Style::spaceSm * m_scale * 2.0F;
      const float badgeWidth = m_headerBadgeLabel->width() + Style::spaceXs * 0.75F * m_scale * 2.0F;
      const float gaps = Style::spaceXs * m_scale * 3.0F;
      const float availableTitleWidth = std::max(0.0F, width - paddingH - iconPx - badgeWidth - actionsWidth - gaps);

      m_headerTitle->setMaxWidth(availableTitleWidth);
      m_headerTitle->measure(renderer);

      m_headerClear->setTooltip(i18n::tr("control-center.notifications.clear-app", "app", row.appDisplayName));
      m_headerClear->setOnClick([onClear = std::move(onClear), appKey = row.appKey] { onClear(appKey); });

      m_headerChevron->setGlyph("chevron-up");
      m_headerChevron->setTooltip(i18n::tr("control-center.notifications.collapse-group", "app", row.appDisplayName));
      m_headerChevron->setOnClick([onToggleExpanded, appKey = row.appKey] { onToggleExpanded(appKey); });
    }

    void bindNotification(
        Renderer& renderer, const HistoryRowItem& row, const NotificationHistoryEntry& entry, float width,
        bool expanded, bool showHistoryActions, IconResolver& iconResolver,
        std::function<void(uint32_t)> onToggleExpanded, std::function<void(const std::string&)> onToggleStackExpanded,
        std::function<void(uint32_t, bool)> onRemove, std::function<void(const std::string&)> onClearApp,
        const std::function<void(uint32_t, const std::string&)>& onAction
    ) {
      if (m_headerImageKind != ImageKind::None) {
        m_headerImage->clear(renderer);
        m_headerImageKind = ImageKind::None;
      }

      m_headerContainer->setVisible(false);
      m_headerContainer->setParticipatesInLayout(false);
      m_notificationContainer->setVisible(true);
      m_notificationContainer->setParticipatesInLayout(true);

      m_isStackedCard = (row.kind == HistoryRowItem::Kind::StackedCard);
      m_shelf2Visible = m_isStackedCard && (row.notificationCount >= 3);

      const bool showAppName = !row.hasAppHeader;
      const NotificationCardMetrics metrics = measureNotificationCard(
          renderer, entry, m_scale, width, expanded, showHistoryActions, m_isStackedCard, showAppName
      );

      const float shelfExtra =
          m_isStackedCard ? (m_shelf2Visible ? kStackShelf2Offset : kStackShelf1Offset) * m_scale : 0.0F;
      const float totalH = metrics.height + shelfExtra;

      setMinWidth(width);
      setSize(width, totalH);
      m_notificationContainer->setSize(width, metrics.height);

      if (m_isStackedCard) {
        m_shelf1->setVisible(true);
        m_shelf2->setVisible(m_shelf2Visible);
        applyShelfLayout();
      } else {
        m_shelf1->setVisible(false);
        m_shelf2->setVisible(false);
      }

      const float iconPx = kHistoryIconSize * m_scale;
      m_iconSlot->setSize(iconPx, iconPx);

      bindIcon(renderer, entry, iconResolver);

      m_meta->setText(metrics.metaLine);
      m_meta->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));

      if (m_isStackedCard) {
        m_stackBadge->setVisible(true);
        m_stackBadge->setParticipatesInLayout(true);
        m_stackBadgeLabel->setText(std::to_string(row.notificationCount));
        m_stackBadgeLabel->measure(renderer);

        const float iconColumn = iconPx + Style::spaceSm * m_scale;
        const float actionButtonSize = kNotificationActionButtonSize * m_scale;
        const float headerActionsWidth = actionButtonSize * 2.0F + Style::spaceXs * m_scale;
        const float leftClusterWidth = metrics.cardTextWidth - headerActionsWidth;
        const float badgeW = m_stackBadgeLabel->width() + Style::spaceXs * m_scale * 2.0F + Style::spaceSm * m_scale;
        m_meta->setMaxWidth(std::max(0.0F, leftClusterWidth - iconColumn - badgeW));
      } else {
        m_stackBadge->setVisible(false);
        m_stackBadge->setParticipatesInLayout(false);
        m_meta->setMaxWidth(metrics.metaTextWidth);
      }
      m_meta->measure(renderer);

      if (m_isStackedCard) {
        m_expand->setVisible(true);
        m_expand->setEnabled(true);
        m_expand->setGlyph("chevron-down");
        m_expand->setTooltip(i18n::tr("control-center.notifications.expand-group", "app", row.appDisplayName));
        m_expand->setOnClick([onToggleStackExpanded = std::move(onToggleStackExpanded), appKey = row.appKey] {
          onToggleStackExpanded(appKey);
        });

        m_dismiss->setTooltip(i18n::tr("control-center.notifications.clear-app", "app", row.appDisplayName));
        m_dismiss->setOnClick([onClearApp = std::move(onClearApp), appKey = row.appKey] { onClearApp(appKey); });
      } else {
        m_expand->setVisible(metrics.canExpand);
        m_expand->setEnabled(metrics.canExpand);
        m_expand->setGlyph(metrics.expanded ? "chevron-up" : "chevron-down");
        m_expand->setTooltip("");
        m_expand->setOnClick([onToggleExpanded = std::move(onToggleExpanded), id = entry.notification.id] {
          onToggleExpanded(id);
        });

        m_dismiss->setTooltip("");
        m_dismiss->setOnClick([onRemove = std::move(onRemove), id = entry.notification.id, active = entry.active] {
          onRemove(id, active);
        });
      }

      m_summary->setText(metrics.summaryText);
      m_summary->setMaxWidth(metrics.cardTextWidth);
      m_summary->setMaxLines(metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines);
      m_summary->measure(renderer);

      if (metrics.bodyText.empty()) {
        m_body->setVisible(false);
        m_body->setText("");
      } else {
        m_body->setVisible(true);
        m_body->setText(metrics.bodyText);
        m_body->setMaxWidth(metrics.cardTextWidth);
        m_body->setMaxLines(metrics.expanded ? kExpandedMaxLines : kBodyMaxLines);
        m_body->measure(renderer);
      }

      while (!m_actionsRow->children().empty()) {
        m_actionsRow->removeChild(m_actionsRow->children().back().get());
      }
      m_actionsRow->setVisible(false);
      if (showHistoryActions && !entry.notification.actions.empty() && !m_isStackedCard) {
        std::vector<std::unique_ptr<Button>> buttons;
        const std::size_t limit = std::min(entry.notification.actions.size(), kMaxNotificationActions * 2);
        for (std::size_t i = 0; i + 1 < limit; i += 2) {
          const std::string& actionKey = entry.notification.actions[i];
          if (actionKey.empty()) {
            continue;
          }
          auto button = ui::button({
              .text = historyActionLabel(actionKey, entry.notification.actions[i + 1]),
              .fontSize = Style::fontSizeCaption * m_scale,
              .variant = ButtonVariant::Default,
          });
          button->setOnClick([onAction, id = entry.notification.id, key = std::string(actionKey)] {
            onAction(id, key);
          });
          buttons.push_back(std::move(button));
        }

        auto rows = wrapButtonsIntoRows(renderer, buttons, metrics.cardTextWidth, Style::spaceXs * m_scale);
        populateRowContainer(*m_actionsRow, std::move(rows), metrics.cardTextWidth, Style::spaceXs * m_scale);

        m_actionsRow->setVisible(!m_actionsRow->children().empty());
      }
    }

  private:
    enum class ImageKind {
      None,
      File,
      Raw,
    };

    static std::unique_ptr<Button> makeActionButton(std::string_view glyph, float scale) {
      return ui::button({
          .glyph = std::string(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = kNotificationActionButtonSize * scale,
          .minHeight = kNotificationActionButtonSize * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
      });
    }

    static std::unique_ptr<Button> makeHeaderActionButton(std::string_view glyph, float scale) {
      return ui::button({
          .glyph = std::string(glyph),
          .glyphSize = kAppHeaderActionGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = kAppHeaderActionButtonSize * scale,
          .minHeight = kAppHeaderActionButtonSize * scale,
          .padding = Style::spaceXs * 0.5F * scale,
          .radius = Style::scaledRadiusSm(scale),
      });
    }

    void showFallbackHeaderIcon(Renderer& renderer) {
      if (m_headerImageKind != ImageKind::None) {
        m_headerImage->clear(renderer);
      }
      m_headerImageKind = ImageKind::None;
      m_headerImage->setVisible(false);

      const float iconPx = kAppHeaderIconSize * m_scale;
      m_headerFallback->setGlyph("bell");
      m_headerFallback->setGlyphSize(kAppHeaderIconGlyphSize * m_scale);
      m_headerFallback->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_headerFallback->measure(renderer);
      m_headerFallback->setPosition(
          std::round((iconPx - m_headerFallback->width()) * 0.5F),
          std::round((iconPx - m_headerFallback->height()) * 0.5F)
      );
      m_headerFallback->setVisible(true);
    }

    void bindHeaderIcon(Renderer& renderer, const NotificationHistoryEntry* repEntry, IconResolver& iconResolver) {
      const float iconPx = kAppHeaderIconSize * m_scale;
      const float iconRadius = notificationIconRadius(kAppHeaderIconSize, m_scale);
      m_headerIconSlot->setRadius(iconRadius);
      m_headerImage->setSize(iconPx, iconPx);
      m_headerImage->setPosition(0.0F, 0.0F);
      m_headerImage->setRadius(iconRadius);
      m_headerImage->setFit(ImageFit::Cover);

      const int targetSize = static_cast<int>(std::round(iconPx));
      if (repEntry != nullptr) {
        const std::string iconPath = resolveHistoryIconPath(repEntry->notification, iconResolver, targetSize);
        if (!iconPath.empty()) {
          const bool ready = m_headerImage->setSourceFile(renderer, iconPath, targetSize);
          if (ready) {
            m_headerImageKind = ImageKind::File;
            m_headerImage->setVisible(true);
            m_headerFallback->setVisible(false);
            return;
          }
        }
        if (repEntry->notification.imageData.has_value()) {
          const auto& image = *repEntry->notification.imageData;
          if (image.width > 0 && image.height > 0 && !image.data.empty()) {
            const bool validImageMetadata = image.bitsPerSample == 8
                && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
            const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
            const bool ready = validImageMetadata
                && m_headerImage->setSourceRaw(
                    renderer, image.data.data(), image.data.size(), image.width, image.height, image.rowStride, format,
                    true
                );
            if (ready) {
              m_headerImageKind = ImageKind::Raw;
              m_headerImage->setVisible(true);
              m_headerFallback->setVisible(false);
              return;
            }
          }
        }
      }

      showFallbackHeaderIcon(renderer);
    }

    void showFallbackIcon(Renderer& renderer) {
      if (m_imageKind != ImageKind::None) {
        m_image->clear(renderer);
      }
      m_imageKind = ImageKind::None;
      m_rawImageKey = 0;
      m_image->setVisible(false);

      const float iconPx = kHistoryIconSize * m_scale;
      m_fallback->setGlyph("bell");
      m_fallback->setGlyphSize(kHistoryIconGlyphSize * m_scale);
      m_fallback->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_fallback->measure(renderer);
      m_fallback->setPosition(
          std::round((iconPx - m_fallback->width()) * 0.5F), std::round((iconPx - m_fallback->height()) * 0.5F)
      );
      m_fallback->setVisible(true);
    }

    void bindIcon(Renderer& renderer, const NotificationHistoryEntry& entry, IconResolver& iconResolver) {
      const float iconPx = kHistoryIconSize * m_scale;
      const float iconRadius = notificationIconRadius(iconPx, m_scale);
      m_iconSlot->setRadius(iconRadius);
      m_image->setSize(iconPx, iconPx);
      m_image->setPosition(0.0F, 0.0F);
      m_image->setRadius(iconRadius);
      m_image->setFit(ImageFit::Cover);

      const int targetSize = static_cast<int>(std::round(iconPx));
      if (entry.notification.imageData.has_value()) {
        const auto& image = *entry.notification.imageData;
        if (image.width > 0 && image.height > 0 && !image.data.empty()) {
          const bool validImageMetadata = image.bitsPerSample == 8
              && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
          const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
          const std::uint64_t key = rawImageKey(entry);
          bool ready = m_imageKind == ImageKind::Raw && m_rawImageKey == key && m_image->hasImage();
          if (!ready && validImageMetadata) {
            ready = m_image->setSourceRaw(
                renderer, image.data.data(), image.data.size(), image.width, image.height, image.rowStride, format, true
            );
          }
          if (ready) {
            m_imageKind = ImageKind::Raw;
            m_rawImageKey = key;
            m_image->setVisible(true);
            m_fallback->setVisible(false);
            return;
          }
        }
      }

      // Fallback: if no snapshot pixels are available, load from the live icon path.
      // This avoids showing stale HyprCap screenshots when the file gets overwritten later.
      const std::string iconPath = resolveHistoryIconPath(entry.notification, iconResolver, targetSize);
      if (!iconPath.empty()) {
        const bool ready = m_image->setSourceFile(renderer, iconPath, targetSize);
        if (ready) {
          m_imageKind = ImageKind::File;
          m_rawImageKey = 0;
          m_image->setVisible(true);
          m_fallback->setVisible(false);
          return;
        }
      }

      showFallbackIcon(renderer);
    }

    float m_scale = 1.0F;

    bool m_isStackedCard = false;
    bool m_shelf2Visible = false;
    Box* m_shelf2 = nullptr;
    Box* m_shelf1 = nullptr;

    Flex* m_headerContainer = nullptr;
    Flex* m_headerLeftCluster = nullptr;
    Box* m_headerIconSlot = nullptr;
    Image* m_headerImage = nullptr;
    Glyph* m_headerFallback = nullptr;
    Label* m_headerTitle = nullptr;
    Flex* m_headerBadge = nullptr;
    Label* m_headerBadgeLabel = nullptr;
    Flex* m_headerRightActions = nullptr;
    Button* m_headerClear = nullptr;
    Button* m_headerChevron = nullptr;
    ImageKind m_headerImageKind = ImageKind::None;

    Flex* m_notificationContainer = nullptr;
    Flex* m_header = nullptr;
    Flex* m_leftCluster = nullptr;
    Box* m_iconSlot = nullptr;
    Image* m_image = nullptr;
    Glyph* m_fallback = nullptr;
    Label* m_meta = nullptr;
    Flex* m_stackBadge = nullptr;
    Label* m_stackBadgeLabel = nullptr;
    Flex* m_headerActions = nullptr;
    Button* m_expand = nullptr;
    Button* m_dismiss = nullptr;
    Label* m_summary = nullptr;
    Label* m_body = nullptr;
    Flex* m_actionsRow = nullptr;
    ImageKind m_imageKind = ImageKind::None;
    std::uint64_t m_rawImageKey = 0;
  };

} // namespace

class NotificationHistoryAdapter final : public VirtualListAdapter {
public:
  NotificationHistoryAdapter(NotificationsTab& owner, float scale, float fillOpacity)
      : m_owner(owner), m_scale(scale), m_fillOpacity(fillOpacity) {}

  [[nodiscard]] std::size_t itemCount() const override { return m_owner.m_rows.size(); }

  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override {
    if (index >= m_owner.m_rows.size()) {
      return static_cast<std::uint64_t>(index);
    }
    const auto& row = m_owner.m_rows[index];
    if (row.kind == HistoryRowItem::Kind::AppHeader) {
      return (std::hash<std::string>{}(row.appKey) << 2U) | 1U;
    }
    if (row.kind == HistoryRowItem::Kind::StackedCard) {
      return (std::hash<std::string>{}(row.appKey) << 2U) | 2U;
    }
    return (static_cast<std::uint64_t>(row.notificationId) << 2U);
  }

  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
    if (index >= m_owner.m_rows.size()) {
      return 0;
    }
    const auto& row = m_owner.m_rows[index];
    if (row.kind == HistoryRowItem::Kind::AppHeader) {
      std::uint64_t revision = std::hash<std::string>{}(row.appKey);
      revision ^= static_cast<std::uint64_t>(row.notificationCount) * 0x9E3779B185EBCA87ULL;
      revision ^= (row.collapsed ? 0xD1B54A32D192ED03ULL : 0x517CC1B727220A95ULL);
      revision ^= static_cast<std::uint64_t>(Style::cornerRadiusScale() * 10000.0F) * 0xC2B2AE3D27D4EB4FULL;
      return revision;
    }
    const NotificationHistoryEntry* found = m_owner.filteredEntry(index);
    if (found == nullptr) {
      return 0;
    }
    const auto& entry = *found;
    const bool isStackedCard = (row.kind == HistoryRowItem::Kind::StackedCard);
    const bool expanded = !isStackedCard && m_owner.m_expandedIds.contains(entry.notification.id);
    std::uint64_t revision = revisionForEntry(entry, expanded, m_owner.m_lastRelativeTimeSlot);
    if (isStackedCard) {
      revision ^= static_cast<std::uint64_t>(row.notificationCount) * 0x7F4A7C15ULL;
    }
    revision ^= static_cast<std::uint64_t>(Style::cornerRadiusScale() * 10000.0F) * 0xC2B2AE3D27D4EB4FULL;
    return revision;
  }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_owner.m_rows.size()) {
      return 1.0F;
    }
    const auto& row = m_owner.m_rows[index];
    if (row.kind == HistoryRowItem::Kind::AppHeader) {
      return measureAppHeader(m_scale);
    }
    const NotificationHistoryEntry* found = m_owner.filteredEntry(index);
    if (found == nullptr) {
      return 1.0F;
    }
    const auto& entry = *found;
    const bool isStackedCard = (row.kind == HistoryRowItem::Kind::StackedCard);
    const bool expanded = !isStackedCard && m_owner.m_expandedIds.contains(entry.notification.id);
    const bool showHistoryActions = !isStackedCard
        && m_owner.m_notifications != nullptr
        && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    const bool showAppName = !row.hasAppHeader;
    const float cardH = measureNotificationCard(
                            renderer, entry, m_scale, width, expanded, showHistoryActions, isStackedCard, showAppName
    )
                            .height;
    if (isStackedCard) {
      const float shelfExtra = (row.notificationCount >= 3 ? kStackShelf2Offset : kStackShelf1Offset) * m_scale;
      return cardH + shelfExtra;
    }
    return cardH;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<NotificationHistoryRow>(m_scale, m_fillOpacity);
  }

  void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (index >= m_owner.m_rows.size()) {
      return;
    }
    auto* rowNode = dynamic_cast<NotificationHistoryRow*>(&item);
    if (rowNode == nullptr) {
      return;
    }
    const auto& row = m_owner.m_rows[index];
    if (row.kind == HistoryRowItem::Kind::AppHeader) {
      const NotificationHistoryEntry* repEntry = m_owner.m_notifications != nullptr
          ? m_owner.m_notifications->findHistoryEntry(row.representativeId)
          : nullptr;
      rowNode->bindHeader(
          renderer, row, repEntry, width, m_owner.m_iconResolver,
          [this](const std::string& appKey) { m_owner.toggleAppGroupExpanded(appKey); },
          [this](const std::string& appKey) { m_owner.clearAppGroup(appKey); }
      );
      return;
    }

    const NotificationHistoryEntry* found = m_owner.filteredEntry(index);
    if (found == nullptr) {
      return;
    }
    const auto& entry = *found;
    const bool isStackedCard = (row.kind == HistoryRowItem::Kind::StackedCard);
    const bool expanded = !isStackedCard && m_owner.m_expandedIds.contains(entry.notification.id);
    const bool showHistoryActions = !isStackedCard
        && m_owner.m_notifications != nullptr
        && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    rowNode->bindNotification(
        renderer, row, entry, width, expanded, showHistoryActions, m_owner.m_iconResolver,
        [this](uint32_t id) { m_owner.toggleNotificationExpanded(id); },
        [this](const std::string& appKey) { m_owner.toggleAppGroupExpanded(appKey); },
        [this](uint32_t id, bool active) { m_owner.removeNotificationEntry(id, active); },
        [this](const std::string& appKey) { m_owner.clearAppGroup(appKey); },
        [this](uint32_t id, const std::string& key) { m_owner.invokeNotificationAction(id, key); }
    );
  }

  [[nodiscard]] bool itemInteractive(std::size_t index) const override {
    if (index >= m_owner.m_rows.size()) {
      return false;
    }
    return notification_grouping::isRowStackToggleInteractive(m_owner.m_rows[index]);
  }

  void onActivate(std::size_t index) override {
    if (index >= m_owner.m_rows.size()) {
      return;
    }
    const auto& row = m_owner.m_rows[index];
    if (!notification_grouping::isRowStackToggleInteractive(row)) {
      return;
    }
    m_owner.toggleAppGroupExpanded(row.appKey);
  }

private:
  NotificationsTab& m_owner;
  float m_scale = 1.0F;
  float m_fillOpacity = 1.0F;
};

NotificationsTab::NotificationsTab(
    NotificationManager* notifications, CompositorPlatform* platform, ConfigService* config
)
    : m_notifications(notifications), m_platform(platform), m_config(config) {}

NotificationsTab::~NotificationsTab() = default;

std::unique_ptr<Flex> NotificationsTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  tab->addChild(
      ui::segmented({
          .out = &m_filter,
          .options =
              std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("control-center.notifications.filter.all")},
                  {.label = i18n::tr("control-center.notifications.filter.today")},
                  {.label = i18n::tr("control-center.notifications.filter.yesterday")},
                  {.label = i18n::tr("control-center.notifications.filter.older")},
              },
          .selectedIndex = m_filterIndex,
          .fontSize = Style::fontSizeCaption,
          .scale = scale,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) {
            if (idx == m_filterIndex) {
              return;
            }

            cancelFilterSlide();
            AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
            if (animations == nullptr || m_list == nullptr) {
              m_filterIndex = idx;
              m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
              if (m_list != nullptr) {
                m_list->scrollView().setScrollOffset(0.0F);
              }
              PanelManager::instance().requestLayout();
              return;
            }

            beginFilterSlideOut(idx);
          },
      })
  );

  m_adapter = std::make_unique<NotificationHistoryAdapter>(*this, scale, panelCardOpacity());

  tab->addChild(
      ui::virtualListView({
          .out = &m_list,
          .contentScale = scale,
          .itemGap = Style::spaceMd * scale,
          .overscanItems = 3,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .configure = [](VirtualListView& list) {
            list.setFillWidth(true);
            list.setFillHeight(true);
          },
      })
  );

  tab->addChild(
      ui::column(
          {
              .out = &m_emptyCard,
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .visible = false,
              .configure =
                  [scale, opacity = panelCardOpacity()](Flex& empty) {
                    applyNotificationCardStyle(empty, scale, opacity);
                    empty.setPadding(Style::spaceLg * scale, Style::spaceMd * scale);
                  },
          },
          ui::label({
              .out = &m_emptyTitle,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
          }),
          ui::label({
              .out = &m_emptyBody,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      )
  );

  return tab;
}

std::unique_ptr<Flex> NotificationsTab::createHeaderActions() {
  const float scale = contentScale();
  const bool dndEnabled = m_notifications != nullptr && m_notifications->doNotDisturb();
  return ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_clearAllButton,
          .glyph = "trash",
          .tooltip = i18n::tr("control-center.notifications.clear-all"),
          .onClick = [this] { clearAllNotifications(); },
          .configure =
              [scale](Button& button) {
                panel_button_style::configureHeaderIconButton(button, scale);
                button.setVariant(ButtonVariant::Destructive);
              },
      }),
      ui::button({
          .out = &m_dndButton,
          .glyph = dndEnabled ? "bell-off" : "bell",
          .selected = dndEnabled,
          .tooltip =
              i18n::tr(dndEnabled ? "control-center.notifications.dnd-off" : "control-center.notifications.dnd-on"),
          .onClick = [this] { toggleDoNotDisturb(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void NotificationsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr || m_filter == nullptr) {
    return;
  }

  if (filterSlideOutActive()) {
    dropVanishedFiltered();
  } else {
    refreshDataSnapshot();
  }
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);

  if (m_list != nullptr && m_filterSlideAnimId == 0 && !m_startFilterSlideIn) {
    m_filterSlideBaseX = m_list->x();
    m_filterSlideBaseY = m_list->y();
  }

  if (m_startFilterSlideIn && m_list != nullptr) {
    m_startFilterSlideIn = false;
    beginFilterSlideIn();
  }
}

void NotificationsTab::doUpdate(Renderer& renderer) {
  if (filterSlideOutActive()) {
    dropVanishedFiltered();
    return;
  }
  if (refreshDataSnapshot() && m_root != nullptr) {
    m_root->layout(renderer);
  }
}

void NotificationsTab::onClose() {
  cancelFilterSlide();
  if (m_list != nullptr) {
    m_list->setAdapter(nullptr);
  }
  m_root = nullptr;
  m_list = nullptr;
  m_emptyCard = nullptr;
  m_emptyTitle = nullptr;
  m_emptyBody = nullptr;
  m_filter = nullptr;
  m_clearAllButton = nullptr;
  m_dndButton = nullptr;
  m_adapter.reset();
  m_appGroups.clear();
  m_rows.clear();
  m_expandedAppKeys.clear();
  m_expandedIds.clear();
  m_lastSerial = 0;
  m_lastRelativeTimeSlot = -1;
  m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
  m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  m_startFilterSlideIn = false;
  m_filterSlideDirection = 0;
  m_filterSlideBaseX = 0.0F;
  m_filterSlideBaseY = 0.0F;
}

void NotificationsTab::cancelFilterSlide() {
  if (m_filterSlideAnimId != 0 && m_list != nullptr) {
    if (AnimationManager* animations = m_list->animationManager(); animations != nullptr) {
      animations->cancel(m_filterSlideAnimId);
    }
    m_filterSlideAnimId = 0;
  }
  m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  m_startFilterSlideIn = false;
  m_filterSlideDirection = 0;
  if (m_list != nullptr) {
    m_list->setPosition(m_filterSlideBaseX, m_filterSlideBaseY);
    m_list->setOpacity(1.0F);
  }
}

bool NotificationsTab::filterSlideOutActive() const { return m_filterSlideAnimId != 0 && !m_startFilterSlideIn; }

void NotificationsTab::applyFilterSlide(float progress, bool slidingIn) {
  if (m_list == nullptr || m_root == nullptr) {
    return;
  }

  const float travel = m_root->width();
  if (travel <= 0.0F) {
    return;
  }

  const auto direction = static_cast<float>(m_filterSlideDirection);
  if (slidingIn) {
    m_list->setPosition(m_filterSlideBaseX + direction * travel * (1.0F - progress), m_filterSlideBaseY);
    m_list->setOpacity(0.7F + 0.3F * progress);
  } else {
    m_list->setPosition(m_filterSlideBaseX - direction * travel * progress, m_filterSlideBaseY);
    m_list->setOpacity(1.0F - 0.3F * progress);
  }
}

void NotificationsTab::beginFilterSlideOut(std::size_t nextIndex) {
  AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
  if (animations == nullptr || m_list == nullptr) {
    m_filterIndex = nextIndex;
    m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
    if (m_list != nullptr) {
      m_list->scrollView().setScrollOffset(0.0F);
    }
    PanelManager::instance().requestLayout();
    return;
  }

  m_pendingFilterIndex = nextIndex;
  m_filterSlideDirection = nextIndex > m_filterIndex ? 1 : -1;
  m_filterSlideBaseX = m_list->x();
  m_filterSlideBaseY = m_list->y();

  PanelManager::instance().requestFrameTick();
  m_filterSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyFilterSlide(progress, false);
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_filterSlideAnimId = 0;
        if (m_pendingFilterIndex != std::numeric_limits<std::size_t>::max()) {
          m_filterIndex = m_pendingFilterIndex;
        }
        m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
        m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
        if (m_list != nullptr) {
          m_list->scrollView().setScrollOffset(0.0F);
        }
        m_startFilterSlideIn = true;
        PanelManager::instance().requestLayout();
      },
      m_list
  );
}

void NotificationsTab::beginFilterSlideIn() {
  AnimationManager* animations = m_list != nullptr ? m_list->animationManager() : nullptr;
  if (animations == nullptr || m_list == nullptr) {
    return;
  }

  m_filterSlideBaseX = m_list->x();
  m_filterSlideBaseY = m_list->y();

  applyFilterSlide(0.0F, true);
  PanelManager::instance().requestFrameTick();
  m_filterSlideAnimId = animations->animate(
      0.0F, 1.0F, static_cast<float>(Style::animFast), Easing::EaseOutCubic,
      [this](float progress) {
        applyFilterSlide(progress, true);
        PanelManager::instance().requestRedraw();
      },
      [this] {
        m_filterSlideAnimId = 0;
        if (m_list != nullptr) {
          m_list->setPosition(m_filterSlideBaseX, m_filterSlideBaseY);
          m_list->setOpacity(1.0F);
        }
      },
      m_list
  );
}

void NotificationsTab::clearAllNotifications() {
  if (m_notifications == nullptr) {
    return;
  }

  std::vector<uint32_t> activeIds;
  activeIds.reserve(m_notifications->all().size());
  for (const auto& notification : m_notifications->all()) {
    activeIds.push_back(notification.id);
  }
  for (const uint32_t id : activeIds) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->clearHistory();
  m_expandedIds.clear();
  m_expandedAppKeys.clear();
  m_appGroups.clear();
  m_rows.clear();
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  auto* panel = dynamic_cast<ControlCenterPanel*>(PanelManager::instance().activePanel());
  if (panel != nullptr && !panel->showsSidebar()) {
    PanelManager::instance().close();
  } else {
    PanelManager::instance().refresh();
  }
}

void NotificationsTab::clearAppGroup(const std::string& appKey) {
  if (m_notifications == nullptr) {
    return;
  }

  const auto it = std::ranges::find_if(m_appGroups, [&](const auto& group) { return group.appKey == appKey; });
  if (it == m_appGroups.end()) {
    return;
  }

  const std::vector<uint32_t> ids = it->notificationIds;
  for (const uint32_t id : ids) {
    const bool wasActive =
        std::ranges::any_of(m_notifications->all(), [id](const auto& notification) { return notification.id == id; });
    if (wasActive) {
      (void)m_notifications->close(id, CloseReason::Dismissed);
    }
    m_notifications->removeHistoryEntry(id);
    m_expandedIds.erase(id);
  }

  m_expandedAppKeys.erase(appKey);

  m_lastSerial = 0;
  syncSnapshotAfterMutation();
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::toggleAppGroupExpanded(const std::string& appKey) {
  if (m_expandedAppKeys.contains(appKey)) {
    m_expandedAppKeys.erase(appKey);
  } else {
    m_expandedAppKeys.insert(appKey);
  }

  rebuildVisualRows();
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

bool NotificationsTab::groupHistoryByApp() const noexcept {
  return m_config == nullptr || m_config->config().notification.groupHistoryByApp;
}

void NotificationsTab::rebuildVisualRows() {
  m_rows = notification_grouping::buildVisualRows(m_appGroups, m_expandedAppKeys, groupHistoryByApp());
}

void NotificationsTab::toggleDoNotDisturb() {
  if (m_notifications == nullptr) {
    return;
  }

  (void)m_notifications->toggleDoNotDisturb();
  syncDndButton();
  PanelManager::instance().refresh();
}

bool NotificationsTab::handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit || !KeybindMatcher::matches(KeybindAction::Delete, sym, modifiers)) {
    return false;
  }
  dropVanishedFiltered();
  const NotificationHistoryEntry* top = filteredEntry(0);
  if (top == nullptr) {
    return false;
  }
  removeNotificationEntry(top->notification.id, top->active);
  return true;
}

void NotificationsTab::removeNotificationEntry(uint32_t id, bool wasActive) {
  if (m_notifications == nullptr) {
    return;
  }

  if (wasActive) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->removeHistoryEntry(id);
  m_expandedIds.erase(id);
  m_lastSerial = 0;
  syncSnapshotAfterMutation();
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::toggleNotificationExpanded(uint32_t id) {
  if (m_expandedIds.contains(id)) {
    m_expandedIds.erase(id);
  } else {
    m_expandedIds.insert(id);
  }

  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::invokeNotificationAction(uint32_t id, const std::string& actionKey) {
  if (m_notifications == nullptr || actionKey.empty() || !m_notifications->hasPendingDBusClose(id)) {
    return;
  }
  const std::string activationToken =
      m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
  if (!m_notifications->invokeAction(id, actionKey, activationToken, true)) {
    kLog.warn("notification history: failed to invoke action '{}' for #{}", actionKey, id);
    return;
  }

  m_lastSerial = 0;
  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

bool NotificationsTab::refreshDataSnapshot() {
  const bool hasHistory = m_notifications != nullptr && !m_notifications->history().empty();
  if (m_clearAllButton != nullptr) {
    m_clearAllButton->setVisible(hasHistory);
  }
  syncDndButton();

  const std::uint64_t serial = m_notifications != nullptr ? m_notifications->changeSerial() : 0;
  const std::int64_t relativeSlot = currentRelativeTimeSlot();
  const bool groupByApp = groupHistoryByApp();
  const bool changed = serial != m_lastSerial
      || relativeSlot != m_lastRelativeTimeSlot
      || m_filterIndex != m_lastRebuildFilterIndex
      || groupByApp != m_lastGroupByApp;
  if (!changed) {
    updateEmptyState(hasHistory, !m_rows.empty());
    return false;
  }

  m_appGroups.clear();
  if (m_notifications != nullptr) {
    std::unordered_map<std::string, std::size_t> groupIndexByKey;
    for (const auto& historyEntry : std::views::reverse(m_notifications->history())) {
      if (matchesHistoryFilter(historyEntry, m_filterIndex)) {
        const std::string key = notification_grouping::groupKey(historyEntry.notification);
        if (!groupByApp) {
          m_appGroups.push_back(
              AppNotificationGroup{
                  .appKey = key,
                  .appDisplayName = notification_grouping::groupDisplayName(historyEntry.notification),
                  .representativeId = historyEntry.notification.id,
                  .notificationIds = {historyEntry.notification.id},
              }
          );
          continue;
        }
        auto it = groupIndexByKey.find(key);
        if (it == groupIndexByKey.end()) {
          const std::size_t idx = m_appGroups.size();
          groupIndexByKey.emplace(key, idx);
          m_appGroups.push_back(
              AppNotificationGroup{
                  .appKey = key,
                  .appDisplayName = notification_grouping::groupDisplayName(historyEntry.notification),
                  .representativeId = historyEntry.notification.id,
                  .notificationIds = {historyEntry.notification.id},
              }
          );
        } else {
          m_appGroups[it->second].notificationIds.push_back(historyEntry.notification.id);
        }
      }
    }
  }

  rebuildVisualRows();

  m_lastSerial = serial;
  m_lastRelativeTimeSlot = relativeSlot;
  m_lastRebuildFilterIndex = m_filterIndex;
  m_lastGroupByApp = groupByApp;

  updateEmptyState(hasHistory, !m_rows.empty());
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  return true;
}

void NotificationsTab::syncDndButton() {
  if (m_dndButton == nullptr) {
    return;
  }

  const bool enabled = m_notifications != nullptr && m_notifications->doNotDisturb();
  m_dndButton->setEnabled(m_notifications != nullptr);
  m_dndButton->setSelected(enabled);
  m_dndButton->setGlyph(enabled ? "bell-off" : "bell");
  m_dndButton->setTooltip(
      i18n::tr(enabled ? "control-center.notifications.dnd-off" : "control-center.notifications.dnd-on")
  );
}

void NotificationsTab::updateEmptyState(bool hasHistory, bool hasFiltered) {
  if (m_list != nullptr) {
    m_list->setVisible(hasFiltered);
  }
  if (m_emptyCard != nullptr) {
    m_emptyCard->setVisible(!hasFiltered);
  }

  if (m_emptyTitle == nullptr || m_emptyBody == nullptr) {
    return;
  }

  if (hasHistory) {
    m_emptyTitle->setText(i18n::tr("control-center.notifications.filter-empty-title"));
    m_emptyBody->setText(i18n::tr("control-center.notifications.filter-empty-body"));
  } else {
    m_emptyTitle->setText(i18n::tr("control-center.notifications.empty-title"));
    m_emptyBody->setText(i18n::tr("control-center.notifications.empty-body"));
  }
}

std::optional<std::size_t> NotificationsTab::filteredIndexForId(uint32_t id) const {
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    if ((m_rows[i].kind == HistoryRowItem::Kind::Notification || m_rows[i].kind == HistoryRowItem::Kind::StackedCard)
        && m_rows[i].notificationId == id) {
      return i;
    }
  }
  return std::nullopt;
}

void NotificationsTab::syncSnapshotAfterMutation() {
  if (filterSlideOutActive()) {
    dropVanishedFiltered();
  } else {
    refreshDataSnapshot();
  }
}

void NotificationsTab::dropVanishedFiltered() {
  if (m_notifications == nullptr) {
    return;
  }
  const auto vanished = [this](uint32_t id) { return m_notifications->findHistoryEntry(id) == nullptr; };
  if (notification_grouping::pruneGroups(m_appGroups, vanished, &m_expandedAppKeys)) {
    rebuildVisualRows();
    if (m_list != nullptr) {
      m_list->notifyDataChanged();
    }
  }
}

const NotificationHistoryEntry* NotificationsTab::filteredEntry(std::size_t index) const {
  if (index >= m_rows.size() || m_notifications == nullptr) {
    return nullptr;
  }
  const auto& row = m_rows[index];
  const uint32_t id = row.kind == HistoryRowItem::Kind::Notification ? row.notificationId : row.representativeId;
  return m_notifications->findHistoryEntry(id);
}

void NotificationsTab::onPanelCardOpacityChanged(float opacity) {
  if (m_filter != nullptr) {
    m_filter->setSurfaceOpacity(opacity);
  }
}
