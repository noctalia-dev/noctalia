#include "shell/control_center/notifications_tab.h"

#include "core/log.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/notification.h"
#include "notification/notification_manager.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
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
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center-notifications");

  constexpr float kHistoryIconSize = 36.0f;
  constexpr float kHistoryIconGlyphSize = 22.0f;
  constexpr float kHistoryIconReferenceSize = 36.0f;

  float notificationIconRadius(float iconSize, float localScale) {
    const float baseRadius = Style::radiusMd * (iconSize / kHistoryIconReferenceSize);
    return std::min(iconSize * 0.5f, Style::scaledRadius(baseRadius, localScale));
  }
  constexpr int kHistoryMaxActionButtons = 2;

  constexpr float kNotificationActionButtonSize = Style::controlHeightSm;

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

  float measureHistoryActionsRowHeight(Renderer& renderer, const std::vector<std::string>& actions, float scale) {
    if (actions.empty()) {
      return 0.0f;
    }
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
    });
    int actionCount = 0;
    for (std::size_t i = 0; i + 1 < actions.size() && actionCount < kHistoryMaxActionButtons; i += 2) {
      const std::string& actionKey = actions[i];
      if (actionKey.empty()) {
        continue;
      }
      row->addChild(
          ui::button({
              .text = historyActionLabel(actionKey, actions[i + 1]),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Outline,
          })
      );
      ++actionCount;
    }
    if (actionCount == 0) {
      return 0.0f;
    }
    row->layout(renderer);
    return row->height();
  }
  constexpr int kSummaryMaxLines = 2;
  constexpr int kBodyMaxLines = 3;
  constexpr int kExpandedMaxLines = 500;

  std::filesystem::path remoteNotificationIconCachePath(std::string_view url) {
    return std::filesystem::path("/tmp") / "noctalia-notification-icons" /
           (std::to_string(std::hash<std::string_view>{}(url)) + ".img");
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

    const std::string localPath = normalizeLocalIconPath(iconValue);
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

  void applyNotificationCardStyle(Flex& card, float scale, float fillOpacity, bool showBorder) {
    applySectionCardStyle(card, scale, fillOpacity, showBorder);
  }

  std::string relativeMetaLine(const Notification& n) {
    if (n.receivedWallClock.has_value()) {
      return formatTimeAgo(*n.receivedWallClock);
    }
    return formatElapsedSince(n.receivedTime);
  }

  std::int64_t currentRelativeTimeSlot() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
               .count() /
           15;
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
      return 0.0f;
    }
    const auto bounds = renderer.measureText(text, fontSize, fontWeight, maxWidth, maxLines);
    return std::max(0.0f, bounds.bottom - bounds.top);
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
    return expandedHeight > collapsedHeight + 0.5f;
  }

  struct NotificationCardMetrics {
    std::string summaryText;
    std::string bodyText;
    std::string metaLine;
    bool canExpand = false;
    bool expanded = false;
    float height = 0.0f;
    float cardTextWidth = 0.0f;
    float metaTextWidth = 0.0f;
  };

  NotificationCardMetrics measureNotificationCard(
      Renderer& renderer, const NotificationHistoryEntry& entry, float scale, float width, bool expandedRequested,
      bool showHistoryActions
  ) {
    NotificationCardMetrics metrics;
    const float cardWidth = std::max(0.0f, width);
    const float cardHorizontalPadding = Style::spaceMd * scale * 2.0f;
    metrics.cardTextWidth = std::max(0.0f, cardWidth - cardHorizontalPadding);
    const std::string summaryText = StringUtils::trimLeadingBlankLines(
        entry.notification.summary.empty() ? i18n::tr("control-center.notifications.untitled")
                                           : entry.notification.summary
    );
    const std::string bodyText = StringUtils::trimLeadingBlankLines(entry.notification.body);
    metrics.summaryText = summaryText;

    const bool summaryExpandable = canExpandText(
        renderer, summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth, kSummaryMaxLines
    );
    bool bodyLineTruncated = false;
    const std::string collapsedBodyText = StringUtils::truncateByLines(bodyText, kBodyMaxLines, &bodyLineTruncated);
    const bool bodyExpandable = bodyLineTruncated || canExpandText(
                                                         renderer, bodyText, Style::fontSizeCaption * scale,
                                                         FontWeight::Normal, metrics.cardTextWidth, kBodyMaxLines
                                                     );
    metrics.canExpand = summaryExpandable || bodyExpandable;
    metrics.expanded = metrics.canExpand && expandedRequested;
    metrics.bodyText = metrics.expanded ? bodyText : collapsedBodyText;

    const float iconPx = kHistoryIconSize * scale;
    const float iconColumn = iconPx + Style::spaceSm * scale;
    const float actionButtonSize = kNotificationActionButtonSize * scale;
    const float actionButtonsGap = Style::spaceXs * scale;
    const float headerActionsWidth =
        actionButtonSize + (metrics.canExpand ? (actionButtonsGap + actionButtonSize) : 0.0f);
    const float leftClusterWidth = metrics.cardTextWidth - headerActionsWidth;
    metrics.metaTextWidth = std::max(0.0f, leftClusterWidth - iconColumn);

    metrics.metaLine = entry.notification.appName + " • " + relativeMetaLine(entry.notification);

    const float metaHeight = measuredTextHeight(
        renderer, metrics.metaLine, Style::fontSizeCaption * scale, FontWeight::Normal, metrics.metaTextWidth, 0
    );
    const float headerHeight = std::max({iconPx, actionButtonSize, metaHeight});
    const float summaryHeight = measuredTextHeight(
        renderer, metrics.summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth,
        metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines
    );
    const float bodyHeight = metrics.bodyText.empty()
                                 ? 0.0f
                                 : measuredTextHeight(
                                       renderer, metrics.bodyText, Style::fontSizeCaption * scale, FontWeight::Normal,
                                       metrics.cardTextWidth, metrics.expanded ? kExpandedMaxLines : kBodyMaxLines
                                   );

    const float actionsRowHeight =
        showHistoryActions ? measureHistoryActionsRowHeight(renderer, entry.notification.actions, scale) : 0.0f;

    const float paddingY = (Style::spaceSm + Style::spaceXs) * scale * 2.0f;
    int visibleSegments = 2;
    if (!metrics.bodyText.empty()) {
      ++visibleSegments;
    }
    if (actionsRowHeight > 0.5f) {
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
  public:
    explicit NotificationHistoryRow(float scale, float fillOpacity, bool showBorder) : m_scale(scale) {
      applyNotificationCardStyle(*this, scale, fillOpacity, showBorder);
      setFillWidth(true);

      m_header = static_cast<Flex*>(addChild(
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
              .flexGrow = 1.0f,
          })
      ));

      m_iconSlot = static_cast<Box*>(m_leftCluster->addChild(
          ui::box({
              .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
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
              .fontSize = Style::fontSizeCaption * scale,
              .flexGrow = 1.0f,
              .configure = [](Label& label) { label.setCaptionStyle(); },
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

      m_summary = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
          })
      ));

      m_body = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      ));

      m_actionsRow = static_cast<Flex*>(addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
              .fillWidth = true,
              .visible = false,
          })
      ));
      for (int i = 0; i < kHistoryMaxActionButtons; ++i) {
        m_actionButtons[static_cast<std::size_t>(i)] = static_cast<Button*>(m_actionsRow->addChild(
            ui::button({
                .fontSize = Style::fontSizeCaption * scale,
                .variant = ButtonVariant::Outline,
                .visible = false,
            })
        ));
      }
    }

    void bind(
        Renderer& renderer, const NotificationHistoryEntry& entry, float width, bool expanded, bool showHistoryActions,
        IconResolver& iconResolver, std::function<void(uint32_t)> onToggleExpanded,
        std::function<void(uint32_t, bool)> onRemove, const std::function<void(uint32_t, const std::string&)>& onAction
    ) {
      const NotificationCardMetrics metrics =
          measureNotificationCard(renderer, entry, m_scale, width, expanded, showHistoryActions);
      setMinWidth(width);
      setSize(width, metrics.height);

      const float iconPx = kHistoryIconSize * m_scale;
      m_iconSlot->setSize(iconPx, iconPx);

      bindIcon(renderer, entry, iconResolver);

      m_meta->setText(metrics.metaLine);
      m_meta->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_meta->setMaxWidth(metrics.metaTextWidth);
      m_meta->measure(renderer);

      m_expand->setVisible(metrics.canExpand);
      m_expand->setEnabled(metrics.canExpand);
      m_expand->setGlyph(metrics.expanded ? "chevron-up" : "chevron-down");
      m_expand->setOnClick([onToggleExpanded = std::move(onToggleExpanded), id = entry.notification.id]() {
        onToggleExpanded(id);
      });

      m_dismiss->setOnClick([onRemove = std::move(onRemove), id = entry.notification.id, active = entry.active]() {
        onRemove(id, active);
      });

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

      for (int ai = 0; ai < kHistoryMaxActionButtons; ++ai) {
        m_actionButtons[static_cast<std::size_t>(ai)]->setVisible(false);
        m_actionButtons[static_cast<std::size_t>(ai)]->setOnClick(nullptr);
      }
      m_actionsRow->setVisible(false);
      if (showHistoryActions) {
        int shownActions = 0;
        for (std::size_t i = 0; i + 1 < entry.notification.actions.size() && shownActions < kHistoryMaxActionButtons;
             i += 2) {
          const std::string& actionKey = entry.notification.actions[i];
          if (actionKey.empty()) {
            continue;
          }
          Button* btn = m_actionButtons[static_cast<std::size_t>(shownActions)];
          btn->setText(historyActionLabel(actionKey, entry.notification.actions[i + 1]));
          btn->setEnabled(true);
          btn->setOnClick([onAction, id = entry.notification.id, key = std::string(actionKey)]() {
            onAction(id, key);
          });
          btn->setVisible(true);
          ++shownActions;
        }
        m_actionsRow->setVisible(shownActions > 0);
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
          std::round((iconPx - m_fallback->width()) * 0.5f), std::round((iconPx - m_fallback->height()) * 0.5f)
      );
      m_fallback->setVisible(true);
    }

    void bindIcon(Renderer& renderer, const NotificationHistoryEntry& entry, IconResolver& iconResolver) {
      const float iconPx = kHistoryIconSize * m_scale;
      const float iconRadius = notificationIconRadius(iconPx, m_scale);
      m_iconSlot->setRadius(iconRadius);
      m_image->setSize(iconPx, iconPx);
      m_image->setPosition(0.0f, 0.0f);
      m_image->setRadius(iconRadius);
      m_image->setFit(ImageFit::Cover);

      const int targetSize = static_cast<int>(std::round(iconPx));
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

      if (entry.notification.imageData.has_value()) {
        const auto& image = *entry.notification.imageData;
        if (image.width > 0 && image.height > 0 && !image.data.empty()) {
          const bool validImageMetadata = image.bitsPerSample == 8 && ((image.channels == 4 && image.hasAlpha) ||
                                                                       (image.channels == 3 && !image.hasAlpha));
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

      showFallbackIcon(renderer);
    }

    float m_scale = 1.0f;
    Flex* m_header = nullptr;
    Flex* m_leftCluster = nullptr;
    Box* m_iconSlot = nullptr;
    Image* m_image = nullptr;
    Glyph* m_fallback = nullptr;
    Label* m_meta = nullptr;
    Flex* m_headerActions = nullptr;
    Button* m_expand = nullptr;
    Button* m_dismiss = nullptr;
    Label* m_summary = nullptr;
    Label* m_body = nullptr;
    Flex* m_actionsRow = nullptr;
    Button* m_actionButtons[kHistoryMaxActionButtons] = {};
    ImageKind m_imageKind = ImageKind::None;
    std::uint64_t m_rawImageKey = 0;
  };

} // namespace

class NotificationHistoryAdapter final : public VirtualListAdapter {
public:
  NotificationHistoryAdapter(NotificationsTab& owner, float scale, float fillOpacity, bool showBorder)
      : m_owner(owner), m_scale(scale), m_fillOpacity(fillOpacity), m_showBorder(showBorder) {}

  [[nodiscard]] std::size_t itemCount() const override { return m_owner.m_filtered.size(); }

  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return static_cast<std::uint64_t>(index);
    }
    return m_owner.m_filtered[index]->notification.id;
  }

  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return 0;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    std::uint64_t revision = revisionForEntry(entry, expanded, m_owner.m_lastRelativeTimeSlot);
    revision ^= static_cast<std::uint64_t>(Style::cornerRadiusScale() * 10000.0f) * 0xC2B2AE3D27D4EB4FULL;
    return revision;
  }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return 1.0f;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    const bool showHistoryActions =
        m_owner.m_notifications != nullptr && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    return measureNotificationCard(renderer, entry, m_scale, width, expanded, showHistoryActions).height;
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<NotificationHistoryRow>(m_scale, m_fillOpacity, m_showBorder);
  }

  void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool /*hovered*/) override {
    if (index >= m_owner.m_filtered.size() || m_owner.m_filtered[index] == nullptr) {
      return;
    }
    auto* row = dynamic_cast<NotificationHistoryRow*>(&item);
    if (row == nullptr) {
      return;
    }
    const auto& entry = *m_owner.m_filtered[index];
    const bool showHistoryActions =
        m_owner.m_notifications != nullptr && m_owner.m_notifications->hasPendingDBusClose(entry.notification.id);
    row->bind(
        renderer, entry, width, m_owner.m_expandedIds.contains(entry.notification.id), showHistoryActions,
        m_owner.m_iconResolver, [this](uint32_t id) { m_owner.toggleNotificationExpanded(id); },
        [this](uint32_t id, bool active) { m_owner.removeNotificationEntry(id, active); },
        [this](uint32_t id, const std::string& key) { m_owner.invokeNotificationAction(id, key); }
    );
  }

private:
  NotificationsTab& m_owner;
  float m_scale = 1.0f;
  float m_fillOpacity = 1.0f;
  bool m_showBorder = false;
};

NotificationsTab::NotificationsTab(NotificationManager* notifications) : m_notifications(notifications) {}

NotificationsTab::~NotificationsTab() = default;

std::unique_ptr<Flex> NotificationsTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
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
          .fontSize = Style::fontSizeCaption * scale,
          .scale = scale,
          .equalSegmentWidths = true,
          .onChange = [this](std::size_t idx) {
            m_filterIndex = idx;
            m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
            if (m_list != nullptr) {
              m_list->scrollView().setScrollOffset(0.0f);
            }
            PanelManager::instance().refresh();
          },
      })
  );

  m_adapter = std::make_unique<NotificationHistoryAdapter>(*this, scale, panelCardOpacity(), panelBordersEnabled());

  tab->addChild(
      ui::virtualListView({
          .out = &m_list,
          .itemGap = Style::spaceMd * scale,
          .overscanItems = 3,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0f,
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
                  [scale, opacity = panelCardOpacity(), borders = panelBordersEnabled()](Flex& empty) {
                    applyNotificationCardStyle(empty, scale, opacity, borders);
                    empty.setPadding(Style::spaceLg * scale, Style::spaceMd * scale);
                  },
          },
          ui::label({
              .out = &m_emptyTitle,
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .fontWeight = FontWeight::Bold,
          }),
          ui::label({
              .out = &m_emptyBody,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .configure = [](Label& label) { label.setCaptionStyle(); },
          })
      )
  );

  return tab;
}

std::unique_ptr<Flex> NotificationsTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_clearAllButton,
          .glyph = "trash",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Destructive,
          .minWidth = Style::controlHeightSm * scale,
          .minHeight = Style::controlHeightSm * scale,
          .padding = Style::spaceXs * scale,
          .onClick = [this]() { clearAllNotifications(); },
      })
  );
}

void NotificationsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr || m_filter == nullptr) {
    return;
  }

  refreshDataSnapshot();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void NotificationsTab::doUpdate(Renderer& renderer) {
  if (refreshDataSnapshot() && m_root != nullptr) {
    m_root->layout(renderer);
  }
}

void NotificationsTab::onClose() {
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
  m_adapter.reset();
  m_filtered.clear();
  m_expandedIds.clear();
  m_lastSerial = 0;
  m_lastRelativeTimeSlot = -1;
  m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
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
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
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
  if (!m_notifications->invokeAction(id, actionKey, true)) {
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
    m_clearAllButton->setEnabled(hasHistory);
  }

  const std::uint64_t serial = m_notifications != nullptr ? m_notifications->changeSerial() : 0;
  const std::int64_t relativeSlot = currentRelativeTimeSlot();
  const bool changed =
      serial != m_lastSerial || relativeSlot != m_lastRelativeTimeSlot || m_filterIndex != m_lastRebuildFilterIndex;
  if (!changed) {
    updateEmptyState(hasHistory, !m_filtered.empty());
    return false;
  }

  m_filtered.clear();
  if (m_notifications != nullptr) {
    m_filtered.reserve(m_notifications->history().size());
    for (auto it = m_notifications->history().rbegin(); it != m_notifications->history().rend(); ++it) {
      if (matchesHistoryFilter(*it, m_filterIndex)) {
        m_filtered.push_back(&*it);
      }
    }
  }

  m_lastSerial = serial;
  m_lastRelativeTimeSlot = relativeSlot;
  m_lastRebuildFilterIndex = m_filterIndex;

  updateEmptyState(hasHistory, !m_filtered.empty());
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  return true;
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
  for (std::size_t i = 0; i < m_filtered.size(); ++i) {
    if (m_filtered[i] != nullptr && m_filtered[i]->notification.id == id) {
      return i;
    }
  }
  return std::nullopt;
}
