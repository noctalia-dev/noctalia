#include "shell/notification/notification_toast.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "net/uri.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/surface/edge_inset.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/layer_surface.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("notification");

  constexpr int kCardWidth = 360;
  constexpr float kInlineReplyInputHeight = Style::controlHeightSm;
  constexpr float kInlineReplyGap = Style::spaceSm;
  constexpr float kInlineReplySendButtonSize = Style::controlHeightSm;

  constexpr float kGap = Style::spaceSm;
  constexpr float kPaddingX = Style::spaceMd;
  constexpr float kPaddingTop = 0.0f;
  constexpr float kPaddingBottom = Style::spaceMd;
  constexpr int kFallbackVisibleCards = 5;
  constexpr float kQueuedY = -1.0f;
  constexpr float kCardInnerPad = Style::spaceMd;
  constexpr float kCloseButtonSize = 20.0f;
  constexpr float kCloseGlyphSize = 12.0f;
  constexpr float kNotificationIconSize = 45.0f;
  constexpr float kNotificationIconSizeCompact = 38.0f;
  constexpr float kNotificationIconGlyphSize = 24.0f;
  constexpr float kNotificationIconGlyphSizeCompact = 20.0f;
  constexpr float kNotificationIconReferenceSize = 36.0f;
  constexpr float kTopProgressInset = Style::spaceMd;
  constexpr auto kExitFallbackGrace = std::chrono::milliseconds(50);

  float notificationIconRadius(float iconSize, float localScale = 1.0f) {
    const float baseRadius = Style::radiusMd * (iconSize / kNotificationIconReferenceSize);
    return std::min(iconSize * 0.5f, Style::scaledRadius(baseRadius, localScale));
  }
  constexpr std::string_view kNoctaliaGlyphIconPrefix = "noctalia-glyph:";
  constexpr float kIconTextGap = Style::spaceSm;
  constexpr float kActionGap = Style::spaceXs;
  constexpr float kActionRowGap = Style::spaceSm;
  std::string fallbackActionLabel() { return i18n::tr("notifications.actions.fallback"); }

  bool hasInlineReplyAction(const std::vector<std::string>& actions) {
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      if (actions[i] == "inline-reply") {
        return true;
      }
    }
    return false;
  }

  std::string inlineReplyPlaceholder(const std::vector<std::string>& actions) {
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      if (actions[i] == "inline-reply") {
        return actions[i + 1];
      }
    }
    return i18n::tr("notifications.inline-reply.placeholder");
  }

  // Maps normalized Notify expire_timeout (see normalizeNotifyExpireTimeout) to toast ms.
  // 0 is persistent; positive values are already the effective server/client timeout.
  int resolveDisplayDuration(int32_t timeout) {
    if (timeout <= 0) {
      return -1;
    }
    return static_cast<int>(timeout);
  }
  constexpr int kProgressHeight = 3;
  constexpr int kContentSlideOffset = 12; // subtle foreground slide during reveal/retract
  constexpr float kMetaFontSize = Style::fontSizeMini;
  constexpr float kSummaryFontSize = Style::fontSizeTitle;
  constexpr float kBodyFontSize = Style::fontSizeBody;
  constexpr int kMaxSummaryLines = 2;
  constexpr int kToastMaxBodyLines = 3;
  constexpr int kMaxToastCardHeight = 320;

  [[nodiscard]] float notificationUiScale(const ConfigService* config) {
    if (config == nullptr) {
      return 1.0f;
    }
    const auto& accessibility = config->config().accessibility;
    const auto& notification = config->config().notification;
    return std::max(0.1f, accessibility.uiScale * notification.scale);
  }

  [[nodiscard]] float cardWidth(float scale) { return static_cast<float>(kCardWidth) * scale; }

  [[nodiscard]] float paddingTop(float scale) { return kPaddingTop * scale; }

  [[nodiscard]] float paddingX(float scale) { return kPaddingX * scale; }

  [[nodiscard]] float paddingBottom(float scale) { return kPaddingBottom * scale; }

  [[nodiscard]] float cardInnerPad(float scale) { return kCardInnerPad * scale; }

  [[nodiscard]] float closeButtonSize(float scale) { return kCloseButtonSize * scale; }

  [[nodiscard]] float notificationIconSize(float scale, bool showActions) {
    return (showActions ? kNotificationIconSize : kNotificationIconSizeCompact) * scale;
  }

  [[nodiscard]] float notificationIconGlyphSize(float scale, bool showActions) {
    return (showActions ? kNotificationIconGlyphSize : kNotificationIconGlyphSizeCompact) * scale;
  }

  [[nodiscard]] float iconTextGap(float scale) { return kIconTextGap * scale; }

  [[nodiscard]] float actionGap(float scale) { return kActionGap * scale; }

  [[nodiscard]] float actionRowGap(float scale) { return kActionRowGap * scale; }

  [[nodiscard]] float progressHeight(float scale) { return static_cast<float>(kProgressHeight) * scale; }

  [[nodiscard]] float topProgressInset(float scale) { return kTopProgressInset * scale; }

  [[nodiscard]] float metaFontSize(float scale) { return kMetaFontSize * scale; }

  [[nodiscard]] float summaryFontSize(float scale) { return kSummaryFontSize * scale; }

  [[nodiscard]] float bodyFontSize(float scale) { return kBodyFontSize * scale; }

  [[nodiscard]] float maxToastCardHeight(float scale) { return static_cast<float>(kMaxToastCardHeight) * scale; }

  [[nodiscard]] std::uint32_t surfaceWidth(float scale, float innerPadX) {
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(cardWidth(scale) + innerPadX * 2.0f))));
  }

  [[nodiscard]] std::uint32_t fallbackSurfaceHeight(float scale) {
    const float totalHeight = maxToastCardHeight(scale) * kFallbackVisibleCards
        + (kGap * scale) * (kFallbackVisibleCards - 1)
        + paddingBottom(scale);
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(totalHeight))));
  }

  float contentOpacityForReveal(float reveal) {
    const float v = std::clamp(reveal, 0.0f, 1.0f);
    if (v <= 0.15f) {
      return 0.0f;
    }
    return std::clamp((v - 0.15f) / 0.85f, 0.0f, 1.0f);
  }

  float contentOffsetForReveal(float reveal, float scale) {
    return std::round(static_cast<float>(kContentSlideOffset) * scale * (1.0f - std::clamp(reveal, 0.0f, 1.0f)));
  }

  float cardRevealFromNode(
      const Node* cardNode, NotificationToast::RevealDirection direction, float cardHeight, float scale
  ) {
    if (cardNode == nullptr) {
      return 0.0f;
    }
    switch (direction) {
    case NotificationToast::RevealDirection::FromLeft:
    case NotificationToast::RevealDirection::FromRight:
      return std::clamp(cardNode->width() / cardWidth(scale), 0.0f, 1.0f);
    case NotificationToast::RevealDirection::FromTop:
    case NotificationToast::RevealDirection::FromBottom:
      return cardHeight > 0.0f ? std::clamp(cardNode->height() / cardHeight, 0.0f, 1.0f) : 0.0f;
    }
    return 0.0f;
  }

  NotificationToast::RevealDirection revealDirectionForPosition(std::string_view position) {
    if (position.ends_with("_left"))
      return NotificationToast::RevealDirection::FromLeft;
    if (position.ends_with("_right"))
      return NotificationToast::RevealDirection::FromRight;
    if (position.starts_with("bottom_"))
      return NotificationToast::RevealDirection::FromBottom;
    return NotificationToast::RevealDirection::FromTop;
  }

  void applyCardRevealNodes(
      Node* cardNode, Node* cardContent, Node* cardForeground, float reveal, float y,
      NotificationToast::RevealDirection direction, float cardHeight, float scale, float edgePadX
  ) {
    if (cardNode == nullptr || cardContent == nullptr || cardForeground == nullptr) {
      return;
    }

    const float clampedReveal = std::clamp(reveal, 0.0f, 1.0f);
    const float contentSlide = contentOffsetForReveal(clampedReveal, scale);

    switch (direction) {
    case NotificationToast::RevealDirection::FromLeft: {
      const float visibleWidth = std::round(cardWidth(scale) * clampedReveal);
      cardNode->setPosition(edgePadX, y);
      cardNode->setFrameSize(visibleWidth, cardHeight);
      cardContent->setPosition(0.0f, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(-contentSlide, 0.0f);
      break;
    }
    case NotificationToast::RevealDirection::FromRight: {
      const float visibleWidth = std::round(cardWidth(scale) * clampedReveal);
      const float hiddenWidth = cardWidth(scale) - visibleWidth;
      cardNode->setPosition(edgePadX + hiddenWidth, y);
      cardNode->setFrameSize(visibleWidth, cardHeight);
      cardContent->setPosition(-hiddenWidth, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(contentSlide, 0.0f);
      break;
    }
    case NotificationToast::RevealDirection::FromTop: {
      const float visibleHeight = std::round(cardHeight * clampedReveal);
      cardNode->setPosition(edgePadX, y);
      cardNode->setFrameSize(cardWidth(scale), visibleHeight);
      cardContent->setPosition(0.0f, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(0.0f, -contentSlide);
      break;
    }
    case NotificationToast::RevealDirection::FromBottom: {
      const float visibleHeight = std::round(cardHeight * clampedReveal);
      const float hiddenHeight = cardHeight - visibleHeight;
      cardNode->setPosition(edgePadX, y + hiddenHeight);
      cardNode->setFrameSize(cardWidth(scale), visibleHeight);
      cardContent->setPosition(0.0f, -hiddenHeight);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(0.0f, contentSlide);
      break;
    }
    }
  }

  std::int32_t outputLogicalHeight(const WaylandOutput& output) { return output.effectiveLogicalHeight(); }

  float notificationTextMaxWidth(float scale, bool showActions) {
    return std::max(
        0.0f,
        cardWidth(scale) - cardInnerPad(scale) * 2.0f - notificationIconSize(scale, showActions) - iconTextGap(scale)
    );
  }

  bool shouldShowNotificationAppName(const ConfigService* config, std::string_view appName) {
    if (StringUtils::isBlank(appName)) {
      return false;
    }
    if (config != nullptr && !config->config().notification.showAppName) {
      return false;
    }
    return true;
  }

  bool shouldShowNotificationActions(const ConfigService* config) {
    return config == nullptr || config->config().notification.showActions;
  }

  std::unique_ptr<Button> makeNotificationActionButton(std::string_view label, float scale) {
    return ui::button({
        .text = std::string(label),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Default,
    });
  }

  ColorSpec toastProgressFillColor(Urgency urgency) {
    switch (urgency) {
    case Urgency::Critical:
      return colorSpecFromRole(ColorRole::Error);
    case Urgency::Low:
      return colorSpecFromRole(ColorRole::OnSurface, 0.9f);
    case Urgency::Normal:
    default:
      return colorSpecFromRole(ColorRole::Primary);
    }
  }

  bool showToastProgressAccent(Urgency urgency, int displayDurationMs) {
    return displayDurationMs >= 0 || urgency == Urgency::Critical;
  }

  std::vector<std::unique_ptr<Button>>
  collectNotificationActionButtons(const std::vector<std::string>& actions, float scale) {
    std::vector<std::unique_ptr<Button>> buttons;
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      const std::string& actionKey = actions[i];
      std::string actionLabel = actions[i + 1];
      if (actionKey.empty() || actionKey == "default") {
        continue;
      }
      if (StringUtils::isBlank(actionLabel)) {
        actionLabel = fallbackActionLabel();
      }
      buttons.push_back(makeNotificationActionButton(actionLabel, scale));
    }
    return buttons;
  }

  float layoutNotificationActionsRow(
      RenderContext& rc, Flex& container, std::vector<std::unique_ptr<Button>>& buttons, float scale
  ) {
    container.setDirection(FlexDirection::Vertical);
    container.setAlign(FlexAlign::Stretch);
    container.setJustify(FlexJustify::Start);
    container.setGap(actionGap(scale));

    const float maxRowWidth = notificationTextMaxWidth(scale, true);

    auto rows = wrapButtonsIntoRows(rc, buttons, maxRowWidth, actionGap(scale));
    populateRowContainer(container, std::move(rows), maxRowWidth, actionGap(scale));

    container.setSize(maxRowWidth, 0.0f);
    container.layout(rc);
    return container.height() + actionRowGap(scale);
  }

  float measureToastCardHeight(
      RenderContext& rc, const ConfigService* config, std::string_view appName, std::string_view summary,
      std::string_view body, const std::vector<std::string>& actions, Urgency urgency, int displayDurationMs,
      int summaryLines, int bodyLines, float scale
  ) {
    const float cardW = cardWidth(scale);
    const float maxCardHeight = maxToastCardHeight(scale);
    const bool showActions = shouldShowNotificationActions(config);
    const float iconSize = notificationIconSize(scale, showActions);
    const float textMaxWidth = notificationTextMaxWidth(scale, showActions);
    const float topTextMaxWidth = std::max(0.0f, textMaxWidth - closeButtonSize(scale) - Style::spaceSm * scale);
    const bool showAppName = shouldShowNotificationAppName(config, appName);

    auto card = ui::column(
        {.maxHeight = maxCardHeight, .clipChildren = true, .width = cardW},
        ui::progressBar({
            .fill = toastProgressFillColor(urgency),
            .track = clearColorSpec(),
            .radius = progressHeight(scale) * 0.5f,
            .orientation = ProgressBarOrientation::HorizontalCentered,
            .width = std::max(0.0f, cardW - topProgressInset(scale) * 2.0f),
            .height = progressHeight(scale),
            .visible = showToastProgressAccent(urgency, displayDurationMs),
            .participatesInLayout = showToastProgressAccent(urgency, displayDurationMs),
            .configure = [scale](ProgressBar& progress) { progress.setPosition(topProgressInset(scale), 0.0f); },
        })
    );

    auto content = ui::row(
        {
            .align = FlexAlign::Start,
            .gap = iconTextGap(scale),
            .padding = cardInnerPad(scale),
            .width = cardW,
        },
        ui::node({
            .width = iconSize,
            .height = iconSize,
        })
    );

    auto textColumn = ui::column(
        {
            .align = FlexAlign::Stretch,
            .width = textMaxWidth,
        },
        ui::label({
            .text = StringUtils::trimLeadingBlankLines(summary),
            .fontSize = summaryFontSize(scale),
            .fontWeight = FontWeight::Bold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxWidth = topTextMaxWidth,
            .maxLines = std::max(1, summaryLines),
        })
    );

    if (bodyLines > 0 && !StringUtils::isBlank(body)) {
      textColumn->addChild(
          ui::label({
              .text = StringUtils::trimLeadingBlankLines(body),
              .fontSize = bodyFontSize(scale),
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxWidth = textMaxWidth,
              .maxLines = std::max(1, bodyLines),
          })
      );
    }

    auto buttons = shouldShowNotificationActions(config) ? collectNotificationActionButtons(actions, scale)
                                                         : std::vector<std::unique_ptr<Button>>{};
    if (!buttons.empty()) {
      auto actionsRow = ui::column({
          .padding = Style::spaceXs * scale,
      });
      layoutNotificationActionsRow(rc, *actionsRow, buttons, scale);
      textColumn->addChild(std::move(actionsRow));
    }

    if (showAppName) {
      textColumn->addChild(
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .justify = FlexJustify::End,
                  .padding = Style::spaceXs * scale,
                  .width = textMaxWidth,
              },
              ui::label({
                  .text = std::string(appName),
                  .fontSize = metaFontSize(scale),
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxWidth = textMaxWidth,
                  .maxLines = 1,
                  .textAlign = TextAlign::End,
              })
          )
      );
    }

    content->addChild(std::move(textColumn));
    card->addChild(std::move(content));
    card->layout(rc);
    return std::ceil(std::min(maxCardHeight, card->height()));
  }

  bool isRemoteIconUrl(std::string_view url) { return uri::isRemoteUrl(url); }

  std::string normalizeLocalIconPath(std::string_view iconValue) { return uri::normalizeFileUrl(iconValue); }

  bool isBottomPosition(std::string_view position) { return position.starts_with("bottom_"); }

  std::uint32_t toastSurfaceAnchor(std::string_view position) {
    if (position.ends_with("_left")) {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    }
    if (position.ends_with("_center")) {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom;
    }
    return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right;
  }

  struct ToastSurfaceMargins {
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::int32_t left = 0;
  };

  ToastSurfaceMargins toastSurfaceMargins(std::string_view position, int offsetX, int offsetY, float scale) {
    const auto sideMargin = shell::surface_edge_inset::resolve(offsetX, paddingX(scale)).layerMargin;
    const auto verticalMargin = static_cast<std::int32_t>(offsetY);
    ToastSurfaceMargins margins{
        .top = verticalMargin,
        .right = sideMargin,
        .bottom = verticalMargin,
        .left = sideMargin,
    };
    if (position.ends_with("_center")) {
      margins.right = 0;
      margins.left = 0;
    }
    return margins;
  }

  std::filesystem::path remoteIconCachePath(std::string_view url) {
    const std::filesystem::path cacheDir = std::filesystem::path("/tmp") / "noctalia-notification-icons";
    const std::size_t hash = std::hash<std::string_view>{}(url);
    return cacheDir / (std::to_string(hash) + ".img");
  }

} // namespace

NotificationToast::NotificationToast() = default;

NotificationToast::~NotificationToast() {
  if (m_notifications != nullptr && m_callbackToken >= 0) {
    m_notifications->removeEventCallback(m_callbackToken);
  }
  destroySurfaces();
}

void NotificationToast::initialize(
    WaylandConnection& wayland, ConfigService* config, NotificationManager* notifications, RenderContext* renderContext,
    HttpClient* httpClient
) {
  m_wayland = &wayland;
  m_config = config;
  m_notifications = notifications;
  m_renderContext = renderContext;
  m_httpClient = httpClient;

  m_callbackToken = m_notifications->addEventCallback([this](const Notification& n, NotificationEvent event) {
    onNotificationEvent(n, event);
  });
}

float NotificationToast::horizontalInnerPad(float scale) const {
  const int offX = m_config != nullptr ? std::max(0, m_config->config().notification.offsetX) : 0;
  return shell::surface_edge_inset::resolve(offX, paddingX(scale)).innerPadding;
}

void NotificationToast::onConfigReload() {
  if (m_entries.empty() && m_instances.empty()) {
    return;
  }
  ensureSurfaces();
  std::vector<bool> wasPlaced(m_entries.size(), false);
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    wasPlaced[i] = hasPlacement(m_entries[i]);
    if (!m_entries[i].exiting) {
      refreshEntryGeometry(m_entries[i]);
    }
  }

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (wasPlaced[i]) {
      m_entries[i].y = kQueuedY;
    }
  }

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    auto& entry = m_entries[i];
    if (entry.exiting || !wasPlaced[i]) {
      continue;
    }
    if (const auto placement = findPlacementY(entry.height); placement.has_value()) {
      entry.y = *placement;
    } else if (entry.rawTimeoutMs > 0 && m_notifications != nullptr) {
      m_notifications->pauseExpiry(entry.notificationId);
    }
  }

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    syncEntryVisibility(i);
  }
  revealQueuedEntries();
  requestLayout();
}

void NotificationToast::onOutputChange() {
  if (m_entries.empty() && m_instances.empty()) {
    return;
  }
  ensureSurfaces();
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    syncEntryVisibility(i);
  }
  requestLayout();
}

void NotificationToast::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->rebuildRequested = true;
      inst->surface->requestLayout();
    }
  }
}

void NotificationToast::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

// --- Notification events ---

void NotificationToast::onNotificationEvent(const Notification& n, NotificationEvent event) {
  switch (event) {
  case NotificationEvent::Added:
    if (m_notifications != nullptr && m_notifications->doNotDisturb()) {
      break;
    }
    m_pendingAdds.push_back(n);
    schedulePendingAdds();
    break;
  case NotificationEvent::Updated: {
    for (auto& pending : m_pendingAdds) {
      if (pending.id == n.id) {
        pending = n;
        return;
      }
    }
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      if (m_entries[i].notificationId == n.id && !m_entries[i].exiting) {
        const bool contentChanged =
            m_entries[i].appName != n.appName || m_entries[i].summary != n.summary || m_entries[i].body != n.body;
        const bool actionSetChanged = (m_entries[i].actions != n.actions) || (m_entries[i].icon != n.icon);
        const bool imageDataChanged = (m_entries[i].imageData != n.imageData);
        const float previousHeight = m_entries[i].height;
        const int prevToastBodyLines = m_entries[i].toastBodyLines;
        const bool previouslyPlaced = hasPlacement(m_entries[i]);
        m_entries[i].appName = notificationDisplayAppName(n);
        m_entries[i].summary = n.summary;
        m_entries[i].body = n.body;
        m_entries[i].actions = n.actions;
        m_entries[i].icon = n.icon;
        m_entries[i].imageData = n.imageData;
        refreshEntryGeometry(m_entries[i]);
        m_entries[i].rawTimeoutMs = n.timeout;
        const bool layoutChanged = contentChanged
            || actionSetChanged
            || imageDataChanged
            || std::abs(previousHeight - m_entries[i].height) > 0.5f
            || prevToastBodyLines != m_entries[i].toastBodyLines;
        const bool hovered = m_entries[i].hovered;

        if (previouslyPlaced) {
          if (canKeepPlacement(m_entries[i], n.id)) {
            evictOverlappingEntries(i);
            if (!canKeepPlacement(m_entries[i], n.id)) {
              m_entries[i].y = kQueuedY;
            }
          } else {
            m_entries[i].y = kQueuedY;
          }
          if (!hasPlacement(m_entries[i]) && m_entries[i].rawTimeoutMs > 0 && m_notifications != nullptr) {
            m_notifications->pauseExpiry(n.id);
          }
        }

        // Update text nodes and reset countdown on each instance
        for (auto& inst : m_instances) {
          if (i >= inst->cards.size()) {
            continue;
          }
          auto& cs = inst->cards[i];
          if (cs.cardNode == nullptr) {
            continue;
          }
          bool regionChanged = false;

          if (layoutChanged) {
            const float preservedReveal = cardReveal(cs, cs.clipHeight);
            const float preservedContentOpacity = cs.cardForeground != nullptr ? cs.cardForeground->opacity() : 1.0f;
            // If the entry reveal animation was still running when this update/replace
            // arrived (e.g. Thunar replacing its USB notification mid-reveal), the rebuilt
            // card would otherwise be frozen at the partial reveal forever — leaving it
            // permanently clipped by the viewport's clipChildren scissor.
            const bool entryRevealInFlight = cs.entryAnimId != 0;

            if (cs.countdownAnimId != 0) {
              inst->animations.cancel(cs.countdownAnimId);
            }
            if (cs.entryAnimId != 0) {
              inst->animations.cancel(cs.entryAnimId);
            }
            if (cs.slideAnimId != 0) {
              inst->animations.cancel(cs.slideAnimId);
            }
            if (cs.exitAnimId != 0) {
              inst->animations.cancel(cs.exitAnimId);
            }

            resetPopupHover(n.id, m_entries[i].displayDurationMs, false);
            if (inst->sceneRoot != nullptr) {
              inst->sceneRoot->removeChild(cs.cardNode);
            }

            cs = {};
            InputArea* rebuilt = buildCard(
                m_entries[i], &cs.cardContent, &cs.cardForeground, &cs.progressBar, &cs.actionsRowNode,
                &cs.inlineReplyRowNode, &cs.inlineReplyInput
            );
            cs.cardNode = rebuilt;
            cs.clipHeight = rebuilt->height();
            const float revealY = hasPlacement(m_entries[i]) ? cardSurfaceY(*inst, i) : 0.0f;
            applyCardReveal(cs, preservedReveal, revealY, cs.clipHeight);
            if (cs.cardForeground != nullptr) {
              cs.cardForeground->setOpacity(preservedContentOpacity);
              cs.cardForeground->setPosition(
                  contentOffsetForReveal(preservedReveal, notificationUiScale(m_config)), 0.0f
              );
            }
            if (inst->sceneRoot != nullptr) {
              inst->sceneRoot->addChild(std::unique_ptr<Node>(rebuilt));
            }
            // Resume an interrupted reveal so the card finishes opening instead of
            // staying scissored at its partial size.
            if (entryRevealInFlight && preservedReveal < 1.0f && hasPlacement(m_entries[i])) {
              const float targetY = cardSurfaceY(*inst, i);
              Instance* instPtr = inst.get();
              cs.entryAnimId = inst->animations.animate(
                  preservedReveal, 1.0f, Style::animNormal, Easing::EaseOutCubic,
                  [this, viewport = cs.cardNode, content = cs.cardContent, foreground = cs.cardForeground, targetY,
                   cardHeight = cs.clipHeight, scale = notificationUiScale(m_config),
                   edgePad = horizontalInnerPad(notificationUiScale(m_config))](float v) {
                    applyCardRevealNodes(
                        viewport, content, foreground, v, targetY, revealDirection(), cardHeight, scale, edgePad
                    );
                  },
                  [this, instPtr, id = n.id]() {
                    if (auto* state = findCardState(*instPtr, id); state != nullptr) {
                      state->entryAnimId = 0;
                    }
                  },
                  cs.cardNode
              );
            }
            regionChanged = true;
          }

          // Reset countdown
          if (cs.countdownAnimId != 0) {
            inst->animations.cancel(cs.countdownAnimId);
          }
          const int newDuration = resolveDisplayDuration(n.timeout);
          m_entries[i].displayDurationMs = newDuration;
          m_entries[i].remainingProgress = 1.0f;
          if (newDuration < 0) {
            cs.progressBar->setOpacity(n.urgency == Urgency::Critical ? 1.0f : 0.0f);
            cs.progressBar->setProgress(1.0f);
            cs.countdownAnimId = 0;
          } else {
            cs.progressBar->setOpacity(1.0f);
            cs.progressBar->setProgress(1.0f);
            if (hovered) {
              cs.countdownAnimId = 0;
            } else {
              cs.countdownAnimId = inst->animations.animateTimer(
                  1.0f, 0.0f, static_cast<float>(newDuration), Easing::Linear,
                  [this, pb = cs.progressBar, notificationId = n.id](float v) {
                    pb->setProgress(v);
                    if (auto* popup = findEntry(notificationId); popup != nullptr) {
                      popup->remainingProgress = v;
                    }
                  },
                  [this, id = n.id]() {
                    DeferredCall::callLater([this, id]() { requestClose(id, CloseReason::Expired); });
                  },
                  cs.progressBar
              );
            }
          }

          // Flash
          if (cs.cardForeground != nullptr) {
            cs.cardForeground->setOpacity(0.7f);
            inst->animations.animate(
                0.7f, 1.0f, Style::animFast, Easing::EaseOutCubic,
                [content = cs.cardForeground](float v) { content->setOpacity(v); }, {}, cs.cardForeground
            );
          }

          // Recompute input + blur regions whenever a card node is rebuilt in place,
          // otherwise the compositor can keep stale strips from the previous geometry.
          if (regionChanged) {
            updateInputRegion(*inst);
            if (inst->pointerInside) {
              inst->inputDispatcher.pointerMotion(inst->lastPointerX, inst->lastPointerY, 0);
            }
          }
          inst->surface->requestRedraw();
        }
        if (hovered && m_notifications != nullptr) {
          m_notifications->pauseExpiry(n.id);
        }

        if (!hasPlacement(m_entries[i])) {
          syncEntryVisibility(i);
          revealQueuedEntries();
        } else if (std::abs(previousHeight - m_entries[i].height) > 0.5f) {
          syncEntryVisibility(i);
          revealQueuedEntries();
        }
        break;
      }
    }
    break;
  }
  case NotificationEvent::Closed:
    std::erase_if(m_pendingAdds, [id = n.id](const Notification& pending) { return pending.id == id; });
    removePopup(n.id);
    break;
  }
}

void NotificationToast::schedulePendingAdds() {
  if (m_pendingAddsScheduled) {
    return;
  }
  m_pendingAddsScheduled = true;
  DeferredCall::callLater([this]() { flushPendingAdds(); });
}

void NotificationToast::flushPendingAdds() {
  m_pendingAddsScheduled = false;
  if (m_pendingAdds.empty()) {
    return;
  }
  auto pending = std::move(m_pendingAdds);
  m_pendingAdds.clear();
  for (const auto& n : pending) {
    addPopup(n);
  }
}

void NotificationToast::addPopup(const Notification& n) {
  for (const auto& entry : m_entries) {
    if (entry.notificationId == n.id) {
      return;
    }
  }

  ensureSurfaces();

  PopupEntry entry;
  entry.notificationId = n.id;
  entry.appName = notificationDisplayAppName(n);
  entry.summary = n.summary;
  entry.body = n.body;
  entry.actions = n.actions;
  entry.icon = n.icon;
  entry.imageData = n.imageData;
  entry.urgency = n.urgency;
  entry.displayDurationMs = resolveDisplayDuration(n.timeout);
  entry.rawTimeoutMs = n.timeout;
  entry.remainingProgress = 1.0f;
  refreshEntryGeometry(entry);
  if (const auto placement = findPlacementY(entry.height); placement.has_value()) {
    entry.y = *placement;
  } else if (entry.rawTimeoutMs > 0 && m_notifications != nullptr) {
    // Queued off-screen: freeze the manager-side auto-dismiss timer so the notification
    // doesn't expire silently before a slot opens up. It will be resumed with the full
    // duration when revealQueuedEntries() places the card.
    m_notifications->pauseExpiry(n.id);
  }
  m_entries.push_back(std::move(entry));
  std::size_t index = m_entries.size() - 1;

  for (auto& inst : m_instances) {
    if (inst->sceneRoot == nullptr) {
      continue;
    }
    inst->cards.resize(m_entries.size());
  }
  syncEntryVisibility(index);
  revealQueuedEntries();

  kLog.debug("notification toast: showing #{}", n.id);
}

void NotificationToast::requestClose(uint32_t notificationId, CloseReason reason) {
  if (m_notifications != nullptr) {
    for (const auto& notification : m_notifications->all()) {
      if (notification.id == notificationId) {
        (void)m_notifications->close(notificationId, reason);
        return;
      }
    }
  }
  removePopup(notificationId);
}

void NotificationToast::removePopup(uint32_t notificationId) {
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (m_entries[i].notificationId == notificationId && !m_entries[i].exiting) {
      dismissPopup(i);
      return;
    }
  }
}

void NotificationToast::dismissPopup(std::size_t index) {
  if (index >= m_entries.size()) {
    return;
  }
  auto& entry = m_entries[index];
  if (entry.exiting) {
    return;
  }
  entry.exiting = true;
  entry.exitFallbackTimer.start(
      std::chrono::milliseconds(Style::animNormal) + kExitFallbackGrace,
      [this, notificationId = entry.notificationId]() {
        if (findEntry(notificationId) != nullptr) {
          finishRemoval(notificationId);
          for (auto& inst : m_instances) {
            if (inst->surface != nullptr) {
              inst->surface->renderNow();
            }
          }
        }
      }
  );

  bool hadVisibleCard = false;
  for (auto& inst : m_instances) {
    if (index < inst->cards.size() && inst->cards[index].cardNode != nullptr) {
      hadVisibleCard = true;
    }
    dismissCardFromInstance(*inst, index);
  }
  if (!hadVisibleCard) {
    finishRemoval(entry.notificationId);
  }
}

void NotificationToast::finishRemoval(uint32_t notificationId) {
  const auto it = std::ranges::find_if(m_entries, [notificationId](const PopupEntry& entry) {
    return entry.notificationId == notificationId;
  });
  if (it == m_entries.end()) {
    return;
  }
  const auto index = static_cast<std::size_t>(std::distance(m_entries.begin(), it));

  // Remove card nodes from all instances
  for (auto& inst : m_instances) {
    if (index < inst->cards.size()) {
      removeCardFromInstance(*inst, index);
      inst->cards.erase(inst->cards.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(index));

  if (m_entries.empty()) {
    destroySurfaces();
  } else {
    if (m_config != nullptr && m_config->config().notification.collapseOnDismiss) {
      collapseStack();
    }
    revealQueuedEntries();
    // A placed entry can overflow a shorter monitor and stay absent there while it fits a
    // taller one. Now that this dismissal freed room (and any collapse repacked the stack),
    // re-check per-instance visibility so it appears on monitors where it now fits.
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      syncEntryVisibility(i);
    }
  }
}

// --- Per-instance card management ---

void NotificationToast::addCardToInstance(Instance& inst, std::size_t entryIndex) {
  auto& entry = m_entries[entryIndex];
  if (!hasPlacement(entry) || !fitsOnSurface(entry, static_cast<float>(inst.surface->height()))) {
    return;
  }

  if (entryIndex >= inst.cards.size()) {
    inst.cards.resize(entryIndex + 1);
  }

  auto& cs = inst.cards[entryIndex];
  cs = {};
  InputArea* card = buildCard(
      entry, &cs.cardContent, &cs.cardForeground, &cs.progressBar, &cs.actionsRowNode, &cs.inlineReplyRowNode,
      &cs.inlineReplyInput
  );
  cs.cardNode = card;
  cs.clipHeight = card->height();

  const float targetY = cardSurfaceY(inst, entryIndex);
  applyCardReveal(cs, 0.0f, targetY, cs.clipHeight);

  inst.sceneRoot->addChild(std::unique_ptr<Node>(card));

  // Entry animation
  cs.entryAnimId = inst.animations.animate(
      0.0f, 1.0f, Style::animNormal, Easing::EaseOutCubic,
      [this, viewport = cs.cardNode, content = cs.cardContent, foreground = cs.cardForeground, targetY,
       cardHeight = cs.clipHeight, scale = notificationUiScale(m_config),
       edgePad = horizontalInnerPad(notificationUiScale(m_config))](float v) {
        applyCardRevealNodes(viewport, content, foreground, v, targetY, revealDirection(), cardHeight, scale, edgePad);
      },
      [this, &inst, id = entry.notificationId]() {
        if (auto* state = findCardState(inst, id); state != nullptr) {
          state->entryAnimId = 0;
        }
      },
      card
  );

  // Countdown. Every instance that hosts a card runs its own countdown animation and
  // calls removePopup when it finishes; dismissPopup's `exiting` guard dedupes. Picking
  // a single "driver" instance is unsafe because addCardToInstance skips instances
  // whose surface can't fit the card, so the nominal driver may never have a card at all.
  if (entry.displayDurationMs < 0) {
    // Persistent — no countdown, no auto-dismiss
    cs.progressBar->setOpacity(entry.urgency == Urgency::Critical ? 1.0f : 0.0f);
    cs.progressBar->setProgress(1.0f);
    cs.countdownAnimId = 0;
  } else {
    const float startProgress = std::clamp(entry.remainingProgress, 0.0f, 1.0f);
    cs.progressBar->setOpacity(1.0f);
    cs.progressBar->setProgress(startProgress);
    if (entry.replyInputFocused || entry.hovered) {
      cs.countdownAnimId = 0;
    } else {
      cs.countdownAnimId = inst.animations.animateTimer(
          startProgress, 0.0f, static_cast<float>(entry.displayDurationMs) * startProgress, Easing::Linear,
          [this, pb = cs.progressBar, notificationId = entry.notificationId](float v) {
            pb->setProgress(v);
            if (auto* popup = findEntry(notificationId); popup != nullptr) {
              popup->remainingProgress = v;
            }
          },
          [this, id = entry.notificationId]() {
            DeferredCall::callLater([this, id]() { requestClose(id, CloseReason::Expired); });
          },
          cs.progressBar
      );
    }
  }

  const int totalDuration = entry.displayDurationMs;
  const uint32_t notificationId = entry.notificationId;
  ProgressBar* progressBarPtr = cs.progressBar;
  InputArea* cardInput = card;
  const bool hasDefaultAction = !entry.actions.empty() && entry.actions.size() >= 2 && entry.actions[0] == "default";

  card->setOnEnter([this, notificationId, progressBarPtr, cardInput, hasDefaultAction](const InputArea::PointerData&) {
    cardInput->setCursorShape(
        hasDefaultAction ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
    );
    beginPopupHover(notificationId, progressBarPtr);
  });

  card->setOnLeave([this, notificationId, totalDuration, progressBarPtr, cardInput]() {
    cardInput->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    endPopupHover(notificationId, totalDuration, progressBarPtr);
  });

  updateInputRegion(inst);
  if (inst.pointerInside) {
    inst.inputDispatcher.pointerMotion(inst.lastPointerX, inst.lastPointerY, 0);
  }
  inst.surface->requestRedraw();
}

void NotificationToast::removeCardFromInstance(Instance& inst, std::size_t entryIndex) {
  if (entryIndex >= inst.cards.size()) {
    return;
  }

  auto& cs = inst.cards[entryIndex];
  if (cs.countdownAnimId != 0) {
    inst.animations.cancel(cs.countdownAnimId);
    cs.countdownAnimId = 0;
  }
  if (cs.entryAnimId != 0) {
    inst.animations.cancel(cs.entryAnimId);
    cs.entryAnimId = 0;
  }
  if (cs.slideAnimId != 0) {
    inst.animations.cancel(cs.slideAnimId);
    cs.slideAnimId = 0;
  }
  if (cs.exitAnimId != 0) {
    inst.animations.cancel(cs.exitAnimId);
    cs.exitAnimId = 0;
  }
  if (cs.cardNode == nullptr) {
    return;
  }

  if (entryIndex < m_entries.size()) {
    resetPopupHover(m_entries[entryIndex].notificationId, m_entries[entryIndex].displayDurationMs, false);
  }

  if (inputAreaBelongsToCard(cs, inst.inputDispatcher.focusedArea())) {
    inst.inputDispatcher.setFocus(nullptr);
  }

  if (inst.sceneRoot != nullptr) {
    (void)inst.sceneRoot->removeChild(cs.cardNode);
  }
  cs = {};

  updateInputRegion(inst);
  syncKeyboardInteractivity(inst);
  if (inst.pointerInside) {
    inst.inputDispatcher.pointerMotion(inst.lastPointerX, inst.lastPointerY, 0);
  }

  if (inst.surface != nullptr) {
    inst.surface->requestRedraw();
  }
}

void NotificationToast::finishExitingEntryIfOrphaned(uint32_t notificationId) {
  const auto it = std::ranges::find_if(m_entries, [notificationId](const PopupEntry& entry) {
    return entry.notificationId == notificationId && entry.exiting;
  });
  if (it == m_entries.end()) {
    return;
  }

  const auto index = static_cast<std::size_t>(std::distance(m_entries.begin(), it));
  for (const auto& inst : m_instances) {
    if (index < inst->cards.size() && inst->cards[index].cardNode != nullptr) {
      return;
    }
  }

  finishRemoval(notificationId);
}

void NotificationToast::syncEntryVisibility(std::size_t entryIndex) {
  if (entryIndex >= m_entries.size()) {
    return;
  }
  if (m_entries[entryIndex].exiting) {
    return;
  }

  for (auto& inst : m_instances) {
    if (inst->sceneRoot == nullptr || inst->surface == nullptr) {
      continue;
    }
    if (entryIndex >= inst->cards.size()) {
      inst->cards.resize(m_entries.size());
    }

    auto& cs = inst->cards[entryIndex];
    const bool shouldShow = hasPlacement(m_entries[entryIndex])
        && fitsOnSurface(m_entries[entryIndex], static_cast<float>(inst->surface->height()));
    if (shouldShow) {
      if (cs.cardNode == nullptr) {
        addCardToInstance(*inst, entryIndex);
      }
    } else if (cs.cardNode != nullptr) {
      removeCardFromInstance(*inst, entryIndex);
    }
  }
}

void NotificationToast::dismissCardFromInstance(Instance& inst, std::size_t entryIndex) {
  if (entryIndex >= inst.cards.size()) {
    return;
  }

  auto& cs = inst.cards[entryIndex];
  if (cs.countdownAnimId != 0) {
    inst.animations.cancel(cs.countdownAnimId);
    cs.countdownAnimId = 0;
  }
  if (cs.entryAnimId != 0) {
    inst.animations.cancel(cs.entryAnimId);
    cs.entryAnimId = 0;
  }
  if (cs.slideAnimId != 0) {
    inst.animations.cancel(cs.slideAnimId);
    cs.slideAnimId = 0;
  }
  if (cs.exitAnimId != 0) {
    inst.animations.cancel(cs.exitAnimId);
    cs.exitAnimId = 0;
  }
  if (cs.cardNode == nullptr) {
    return;
  }

  Node* card = cs.cardNode;
  Node* content = cs.cardContent;
  Node* foreground = cs.cardForeground;
  const float cardHeight = cs.clipHeight > 0.0f ? cs.clipHeight : card->height();
  const float startReveal = cardReveal(cs, cardHeight);
  const float targetY = card->y();
  const uint32_t removingId = (entryIndex < m_entries.size()) ? m_entries[entryIndex].notificationId : 0;

  cs.exitAnimId = inst.animations.animate(
      startReveal, 0.0f, Style::animNormal, Easing::EaseInOutQuad,
      [this, card, content, foreground, targetY, cardHeight, scale = notificationUiScale(m_config),
       edgePad = horizontalInnerPad(notificationUiScale(m_config))](float v) {
        applyCardRevealNodes(card, content, foreground, v, targetY, revealDirection(), cardHeight, scale, edgePad);
      },
      [this, &inst, removingId]() {
        if (removingId != 0) {
          if (auto* state = findCardState(inst, removingId); state != nullptr) {
            state->exitAnimId = 0;
          }
          DeferredCall::callLater([this, removingId]() { finishRemoval(removingId); });
        }
      },
      card
  );

  updateInputRegion(inst);
  inst.surface->requestRedraw();
}

NotificationToast::PopupEntry* NotificationToast::findEntry(uint32_t notificationId) {
  const auto it = std::ranges::find_if(m_entries, [notificationId](const PopupEntry& entry) {
    return entry.notificationId == notificationId;
  });
  if (it == m_entries.end()) {
    return nullptr;
  }
  return &*it;
}

NotificationToast::Instance::CardState* NotificationToast::findCardState(Instance& inst, uint32_t notificationId) {
  for (std::size_t i = 0; i < inst.cards.size() && i < m_entries.size(); ++i) {
    if (m_entries[i].notificationId == notificationId) {
      return &inst.cards[i];
    }
  }
  return nullptr;
}

void NotificationToast::beginPopupHover(uint32_t notificationId, const ProgressBar* progressBar) {
  auto* popup = findEntry(notificationId);
  if (popup == nullptr) {
    return;
  }

  popup->hoverResetPending = false;
  popup->hoverResetToken += 1;
  popup->hoverOwners += 1;
  if (popup->hoverOwners == 1 && !popup->hovered) {
    popup->hovered = true;
    pauseTimeout(notificationId, progressBar);
  } else if (progressBar != nullptr) {
    popup->remainingProgress = std::clamp(progressBar->progress(), 0.0f, 1.0f);
  }
}

void NotificationToast::endPopupHover(uint32_t notificationId, int totalDuration, const ProgressBar* progressBar) {
  auto* popup = findEntry(notificationId);
  if (popup == nullptr) {
    return;
  }

  popup->hoverOwners = std::max(0, popup->hoverOwners - 1);
  if (progressBar != nullptr) {
    popup->remainingProgress = std::clamp(progressBar->progress(), 0.0f, 1.0f);
  }
  if (popup->hoverOwners > 0) {
    return;
  }

  popup->hoverResetPending = true;
  const std::uint64_t resetToken = ++popup->hoverResetToken;
  DeferredCall::callLater([this, notificationId, totalDuration, resetToken]() {
    auto* deferredPopup = findEntry(notificationId);
    if (deferredPopup == nullptr) {
      return;
    }
    if (!deferredPopup->hoverResetPending
        || deferredPopup->hoverResetToken != resetToken
        || deferredPopup->hoverOwners > 0) {
      return;
    }
    resetPopupHover(notificationId, totalDuration, true);
  });
}

void NotificationToast::resetPopupHover(uint32_t notificationId, int totalDuration, bool resumeTimer) {
  auto* popup = findEntry(notificationId);
  if (popup == nullptr) {
    return;
  }

  popup->hoverOwners = 0;
  popup->hoverResetPending = false;
  popup->hoverResetToken += 1;
  popup->hovered = false;
  if (resumeTimer) {
    resumeTimeout(notificationId, totalDuration);
  }
  if (m_config != nullptr && m_config->config().notification.collapseOnDismiss) {
    collapseStack();
  }
  revealQueuedEntries();
}

void NotificationToast::resetInstanceHover(Instance& inst, bool resumeTimers) {
  for (std::size_t i = 0; i < inst.cards.size() && i < m_entries.size(); ++i) {
    if (inst.cards[i].cardNode == nullptr) {
      continue;
    }
    if (!m_entries[i].hovered && m_entries[i].hoverOwners == 0) {
      continue;
    }
    resetPopupHover(m_entries[i].notificationId, m_entries[i].displayDurationMs, resumeTimers);
  }
}

void NotificationToast::pauseTimeout(uint32_t notificationId, const ProgressBar* progressBar) {
  if (auto* popup = findEntry(notificationId); popup != nullptr && progressBar != nullptr) {
    popup->remainingProgress = std::clamp(progressBar->progress(), 0.0f, 1.0f);
  }
  pauseCountdowns(notificationId);
  if (m_notifications != nullptr) {
    m_notifications->pauseExpiry(notificationId);
  }
}

void NotificationToast::resumeTimeout(uint32_t notificationId, int totalDuration) {
  if (totalDuration < 0) {
    return;
  }
  const auto* popup = findEntry(notificationId);
  if (popup == nullptr || popup->hovered || popup->replyInputFocused) {
    return;
  }
  const float remaining = std::clamp(popup->remainingProgress, 0.0f, 1.0f);
  if (remaining <= 0.0f) {
    if (m_notifications != nullptr) {
      m_notifications->resumeExpiry(notificationId, 0);
    }
    return;
  }
  const int32_t remainingMs =
      std::max<int32_t>(1, static_cast<int32_t>(std::ceil(static_cast<float>(totalDuration) * remaining)));
  if (m_notifications != nullptr) {
    m_notifications->resumeExpiry(notificationId, remainingMs);
  }
  resumeCountdowns(notificationId);
}

void NotificationToast::pauseCountdowns(uint32_t notificationId) {
  auto* entry = findEntry(notificationId);
  float remaining = (entry != nullptr) ? std::clamp(entry->remainingProgress, 0.0f, 1.0f) : 1.0f;

  for (auto& inst : m_instances) {
    auto* state = findCardState(*inst, notificationId);
    if (state == nullptr) {
      continue;
    }
    if (state->progressBar != nullptr) {
      remaining = std::clamp(state->progressBar->progress(), 0.0f, 1.0f);
    }
    if (state->countdownAnimId == 0) {
      continue;
    }
    inst->animations.cancel(state->countdownAnimId);
    state->countdownAnimId = 0;
  }

  if (entry != nullptr) {
    entry->remainingProgress = remaining;
  }
}

void NotificationToast::resumeCountdowns(uint32_t notificationId) {
  auto* entry = findEntry(notificationId);
  if (entry == nullptr || entry->displayDurationMs < 0 || entry->hovered || entry->replyInputFocused) {
    return;
  }

  const float remaining = std::clamp(entry->remainingProgress, 0.0f, 1.0f);
  if (remaining <= 0.0f) {
    return;
  }

  for (auto& inst : m_instances) {
    auto* state = findCardState(*inst, notificationId);
    if (state == nullptr || state->progressBar == nullptr) {
      continue;
    }
    if (state->countdownAnimId != 0) {
      inst->animations.cancel(state->countdownAnimId);
      state->countdownAnimId = 0;
    }

    state->progressBar->setOpacity(1.0f);
    state->progressBar->setProgress(remaining);
    const bool isDriver = (!m_instances.empty() && m_instances[0].get() == inst.get());
    state->countdownAnimId = inst->animations.animateTimer(
        remaining, 0.0f, static_cast<float>(entry->displayDurationMs) * remaining, Easing::Linear,
        [this, progressBar = state->progressBar, notificationId](float v) {
          progressBar->setProgress(v);
          if (auto* popup = findEntry(notificationId); popup != nullptr) {
            popup->remainingProgress = v;
          }
        },
        [this, notificationId, isDriver]() {
          if (isDriver) {
            DeferredCall::callLater([this, notificationId]() { requestClose(notificationId, CloseReason::Expired); });
          }
        },
        state->progressBar
    );
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void NotificationToast::revealQueuedEntries() {
  bool placed = false;
  do {
    placed = false;
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      auto& entry = m_entries[i];
      if (entry.exiting || hasPlacement(entry)) {
        continue;
      }
      const auto placement = findPlacementY(entry.height);
      if (!placement.has_value()) {
        continue;
      }
      entry.y = *placement;
      // Restart the manager-side expiry with the full duration now that the card is
      // actually visible. Matches displayDurationMs so the progress bar and the
      // manager timer finish together.
      if (entry.rawTimeoutMs > 0 && m_notifications != nullptr) {
        m_notifications->resumeExpiry(entry.notificationId, entry.rawTimeoutMs);
      }
      syncEntryVisibility(i);
      placed = true;
    }
  } while (placed);
}

void NotificationToast::evictOverlappingEntries(std::size_t anchorIndex) {
  if (anchorIndex >= m_entries.size() || !hasPlacement(m_entries[anchorIndex])) {
    return;
  }

  const float anchorTop = m_entries[anchorIndex].y;
  const float anchorBottom = anchorTop + m_entries[anchorIndex].height;
  const float layoutGap = kGap * notificationUiScale(m_config);

  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (i == anchorIndex || m_entries[i].exiting || !hasPlacement(m_entries[i])) {
      continue;
    }

    const float entryTop = m_entries[i].y;
    const float entryBottom = entryTop + m_entries[i].height;
    const bool separated =
        (entryBottom + layoutGap <= anchorTop + 0.5f) || (anchorBottom + layoutGap <= entryTop + 0.5f);
    if (separated) {
      continue;
    }

    m_entries[i].y = kQueuedY;
    if (m_entries[i].rawTimeoutMs > 0 && m_notifications != nullptr) {
      m_notifications->pauseExpiry(m_entries[i].notificationId);
    }
    syncEntryVisibility(i);
  }
}

bool NotificationToast::hasPlacement(const PopupEntry& entry) const { return !entry.exiting && entry.y >= 0.0f; }

bool NotificationToast::canKeepPlacement(const PopupEntry& entry, std::optional<uint32_t> ignoreNotificationId) const {
  if (!hasPlacement(entry) || entry.y + entry.height > maxPlacementBottom() + 0.5f) {
    return false;
  }

  const float top = entry.y;
  const float bottom = entry.y + entry.height;
  const float layoutGap = kGap * notificationUiScale(m_config);
  for (const auto& other : m_entries) {
    if (!hasPlacement(other)) {
      continue;
    }
    if (other.notificationId == entry.notificationId) {
      continue;
    }
    if (ignoreNotificationId.has_value() && other.notificationId == *ignoreNotificationId) {
      continue;
    }

    const float otherTop = other.y;
    const float otherBottom = other.y + other.height;
    const bool separated = (bottom + layoutGap <= otherTop + 0.5f) || (otherBottom + layoutGap <= top + 0.5f);
    if (!separated) {
      return false;
    }
  }

  return true;
}

bool NotificationToast::fitsOnSurface(const PopupEntry& entry, float surfaceHeight) const {
  if (!hasPlacement(entry)) {
    return false;
  }
  if (surfaceHeight <= 0.5f) {
    return false;
  }
  if (isBottomStacking()) {
    // Placement Y is stored in the tallest monitor's coordinate space; map from the
    // bottom edge so shorter outputs still host the same stack.
    return entryOffsetFromPlacementBottom(entry) <= layoutBottomForSurfaceHeight(surfaceHeight) + 0.5f;
  }
  return entry.y + entry.height <= layoutBottomForSurfaceHeight(surfaceHeight) + 0.5f;
}

std::string NotificationToast::notificationPosition() const {
  if (m_config == nullptr || m_config->config().notification.position.empty()) {
    return "top_right";
  }
  return m_config->config().notification.position;
}

std::string NotificationToast::notificationLayer() const {
  if (m_config == nullptr) {
    return "top";
  }
  const std::string& configured = m_config->config().notification.layer;
  if (configured == "overlay") {
    return "overlay";
  }
  return "top";
}

std::vector<std::string> NotificationToast::notificationMonitors() const {
  if (m_config == nullptr) {
    return {};
  }
  return m_config->config().notification.monitors;
}

bool NotificationToast::shouldRenderOnOutput(const WaylandOutput& output) const {
  const auto selectedMonitors = notificationMonitors();
  if (selectedMonitors.empty()) {
    return true;
  }
  return std::ranges::any_of(selectedMonitors, [&output](const std::string& match) {
    return outputMatchesSelector(match, output);
  });
}

bool NotificationToast::isBottomStacking() const { return isBottomPosition(notificationPosition()); }

NotificationToast::RevealDirection NotificationToast::revealDirection() const {
  return revealDirectionForPosition(notificationPosition());
}

void NotificationToast::refreshEntryGeometry(PopupEntry& entry) const {
  if (m_renderContext == nullptr) {
    entry.toastBodyLines = StringUtils::isBlank(entry.body) ? 0 : kToastMaxBodyLines;
    entry.height = maxToastCardHeight(notificationUiScale(m_config));
    return;
  }

  const float scale = notificationUiScale(m_config);
  entry.toastBodyLines = StringUtils::isBlank(entry.body) ? 0 : kToastMaxBodyLines;
  entry.height = std::min(
      maxToastCardHeight(scale),
      measureToastCardHeight(
          *m_renderContext, m_config, entry.appName, entry.summary, entry.body, entry.actions, entry.urgency,
          entry.displayDurationMs, kMaxSummaryLines, entry.toastBodyLines, scale
      )
  );
}

float NotificationToast::layoutBottomForSurfaceHeight(float surfaceHeight) const {
  const float scale = notificationUiScale(m_config);
  const float edgePadding = isBottomStacking() ? 0.0f : paddingBottom(scale);
  return std::max(paddingTop(scale), surfaceHeight - edgePadding);
}

float NotificationToast::entryOffsetFromPlacementBottom(const PopupEntry& entry) const {
  return maxPlacementBottom() - entry.y;
}

float NotificationToast::cardSurfaceY(const Instance& inst, std::size_t entryIndex) const {
  if (entryIndex >= m_entries.size() || inst.surface == nullptr) {
    return 0.0f;
  }
  const float scale = notificationUiScale(m_config);
  const float layoutGap = kGap * scale;
  const bool bottom = isBottomStacking();
  const auto surfaceHeight = static_cast<float>(inst.surface->height());
  const float layoutBottom = layoutBottomForSurfaceHeight(surfaceHeight);
  const float placementBottom = maxPlacementBottom();

  // Collect this instance's present cards in from-the-edge stacking order. The "primary"
  // coordinate is the distance along the stacking direction from the anchored edge,
  // matching collapseStack(): for bottom stacking it is measured from the placement bottom.
  struct Item {
    std::size_t index;
    float primary;
    float sharedHeight;
  };
  std::vector<Item> items;
  items.reserve(m_entries.size());
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    const auto& entry = m_entries[i];
    if (!hasPlacement(entry)) {
      continue;
    }
    if (i >= inst.cards.size() || inst.cards[i].cardNode == nullptr) {
      continue;
    }
    const float primary = bottom ? (placementBottom - (entry.y + entry.height)) : entry.y;
    items.push_back({i, primary, entry.height});
  }
  if (items.empty()) {
    return bottom ? layoutBottom : paddingTop(scale);
  }
  std::ranges::sort(items, {}, &Item::primary);

  float cursor = items.front().primary;
  for (std::size_t k = 0; k < items.size(); ++k) {
    const float realHeight = inst.cards[items[k].index].clipHeight;
    if (items[k].index == entryIndex) {
      return bottom ? (layoutBottom - cursor - realHeight) : cursor;
    }
    // Preserve the gap the shared skeleton placed after this card (kGap normally, larger
    // when hover or a dismissed-but-not-collapsed slot left extra space), but advance by
    // the real height so the next card sits exactly below/above the previous one.
    float gapAfter = layoutGap;
    if (k + 1 < items.size()) {
      gapAfter = std::max(layoutGap, items[k + 1].primary - (items[k].primary + items[k].sharedHeight));
    }
    cursor += realHeight + gapAfter;
  }
  return bottom ? (layoutBottom - cursor) : cursor;
}

float NotificationToast::maxPlacementBottom() const {
  float maxSurfaceHeight = 0.0f;
  bool haveSurfaceHeight = false;
  for (const auto& inst : m_instances) {
    if (inst != nullptr && inst->surface != nullptr && inst->surface->height() > 0) {
      haveSurfaceHeight = true;
      maxSurfaceHeight = std::max(maxSurfaceHeight, static_cast<float>(inst->surface->height()));
    }
  }
  if (!haveSurfaceHeight && m_wayland != nullptr) {
    for (const auto& output : m_wayland->outputs()) {
      if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
        continue;
      }
      if (!shouldRenderOnOutput(output)) {
        continue;
      }
      haveSurfaceHeight = true;
      maxSurfaceHeight = std::max(maxSurfaceHeight, static_cast<float>(surfaceHeightForOutput(output.output)));
    }
  }
  if (!haveSurfaceHeight) {
    maxSurfaceHeight = static_cast<float>(fallbackSurfaceHeight(notificationUiScale(m_config)));
  }
  return layoutBottomForSurfaceHeight(maxSurfaceHeight);
}

void NotificationToast::alignBottomStackToPlacementBottom() {
  if (!isBottomStacking()) {
    return;
  }

  bool havePlacedEntry = false;
  float stackBottom = 0.0f;
  for (const auto& entry : m_entries) {
    if (!hasPlacement(entry)) {
      continue;
    }
    const float entryBottom = entry.y + entry.height;
    if (!havePlacedEntry || entryBottom > stackBottom) {
      havePlacedEntry = true;
      stackBottom = entryBottom;
    }
  }
  if (!havePlacedEntry) {
    return;
  }

  const float scale = notificationUiScale(m_config);
  const float topPadding = paddingTop(scale);
  const float delta = maxPlacementBottom() - stackBottom;
  if (std::abs(delta) <= 0.5f) {
    return;
  }

  for (auto& entry : m_entries) {
    if (!hasPlacement(entry)) {
      continue;
    }
    entry.y += delta;
    if (entry.y < topPadding - 0.5f) {
      entry.y = kQueuedY;
      if (entry.rawTimeoutMs > 0 && m_notifications != nullptr) {
        m_notifications->pauseExpiry(entry.notificationId);
      }
    }
  }
}

void NotificationToast::collapseStack() {
  const float scale = notificationUiScale(m_config);
  const float layoutGap = kGap * scale;
  const float topPad = paddingTop(scale);
  const float placementBottom = maxPlacementBottom();

  struct PlacedEntry {
    std::size_t index;
    float oldPrimary;
    float newPrimary;
    bool hovered;
  };
  std::vector<PlacedEntry> placed;
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (hasPlacement(m_entries[i]) && !m_entries[i].exiting) {
      const float oldPrimary =
          isBottomStacking() ? placementBottom - (m_entries[i].y + m_entries[i].height) : m_entries[i].y;
      placed.push_back({i, oldPrimary, oldPrimary, m_entries[i].hovered});
    }
  }
  if (placed.empty()) {
    return;
  }

  std::ranges::sort(placed, {}, &PlacedEntry::oldPrimary);

  const float initialCursor = isBottomStacking() ? 0.0f : topPad;
  float cursor = initialCursor;
  for (auto& p : placed) {
    if (p.hovered) {
      cursor = std::max(cursor, p.oldPrimary + m_entries[p.index].height + layoutGap);
      continue;
    }

    p.newPrimary = cursor;
    cursor = cursor + m_entries[p.index].height + layoutGap;
  }

  for (auto& p : placed) {
    if (isBottomStacking()) {
      m_entries[p.index].y = placementBottom - p.newPrimary - m_entries[p.index].height;
    } else {
      m_entries[p.index].y = p.newPrimary;
    }
  }

  for (auto& p : placed) {
    const float newY = m_entries[p.index].y;
    const float oldY = isBottomStacking() ? placementBottom - p.oldPrimary - m_entries[p.index].height : p.oldPrimary;
    if (std::abs(newY - oldY) < 0.5f) {
      continue;
    }

    for (auto& inst : m_instances) {
      if (p.index >= inst->cards.size()) {
        continue;
      }
      auto& cs = inst->cards[p.index];
      if (cs.cardNode == nullptr) {
        continue;
      }

      if (cs.slideAnimId != 0) {
        inst->animations.cancel(cs.slideAnimId);
        cs.slideAnimId = 0;
      }

      const float newSurfY = cardSurfaceY(*inst, p.index);

      // The completion callbacks must NOT capture `cs` by reference: a sibling
      // card's finishRemoval() can erase an earlier slot in inst->cards while this
      // animation is still live, shifting/reallocating the vector and leaving a
      // dangling reference. Re-look-up the slot by notification id instead (the
      // owner-node cancel only fires if *this* card's node is destroyed).
      Instance* instPtr = inst.get();
      const uint32_t entryId = m_entries[p.index].notificationId;

      if (cs.entryAnimId != 0) {
        const float currentReveal = cardReveal(cs, cs.clipHeight);
        inst->animations.cancel(cs.entryAnimId);
        cs.entryAnimId = 0;
        const float cardHeight = cs.clipHeight;
        Node* viewport = cs.cardNode;
        Node* content = cs.cardContent;
        Node* foreground = cs.cardForeground;
        cs.entryAnimId = inst->animations.animate(
            currentReveal, 1.0f, Style::animNormal, Easing::EaseOutCubic,
            [this, viewport, content, foreground, newSurfY, cardHeight, scale,
             edgePad = horizontalInnerPad(scale)](float v) {
              applyCardRevealNodes(
                  viewport, content, foreground, v, newSurfY, revealDirection(), cardHeight, scale, edgePad
              );
            },
            [this, instPtr, entryId]() {
              if (auto* state = findCardState(*instPtr, entryId); state != nullptr) {
                state->entryAnimId = 0;
              }
            },
            viewport
        );
        continue;
      }

      const float px = horizontalInnerPad(scale);
      const float oldSurfY = cs.cardNode->y();
      Node* cardNode = cs.cardNode;

      cs.slideAnimId = inst->animations.animate(
          oldSurfY, newSurfY, Style::animNormal, Easing::EaseInOutQuad,
          [cardNode, px](float v) { cardNode->setPosition(px, v); },
          [this, instPtr, entryId]() {
            if (auto* state = findCardState(*instPtr, entryId); state != nullptr) {
              state->slideAnimId = 0;
            }
          },
          cardNode
      );
    }
  }
}

std::optional<float>
NotificationToast::findPlacementY(float candidateHeight, std::optional<uint32_t> ignoreNotificationId) const {
  struct Interval {
    float top = 0.0f;
    float bottom = 0.0f;
  };

  std::vector<Interval> occupied;
  occupied.reserve(m_entries.size());
  for (const auto& entry : m_entries) {
    if (!hasPlacement(entry)) {
      continue;
    }
    if (ignoreNotificationId.has_value() && entry.notificationId == *ignoreNotificationId) {
      continue;
    }
    occupied.push_back({entry.y, entry.y + entry.height});
  }
  const float bottom = maxPlacementBottom();
  const float scale = notificationUiScale(m_config);
  const float layoutGap = kGap * scale;
  const float topPadding = paddingTop(scale);
  if (isBottomStacking()) {
    std::ranges::sort(occupied, std::ranges::greater{}, &Interval::bottom);
    float cursorBottom = bottom;
    for (const auto& interval : occupied) {
      const float candidateTop = cursorBottom - candidateHeight;
      if (candidateTop >= interval.bottom + layoutGap - 0.5f) {
        return candidateTop;
      }
      cursorBottom = std::min(cursorBottom, interval.top - layoutGap);
    }
    const float candidateTop = cursorBottom - candidateHeight;
    if (candidateTop >= topPadding - 0.5f) {
      return candidateTop;
    }
    return std::nullopt;
  }

  std::ranges::sort(occupied, {}, &Interval::top);
  float cursor = topPadding;
  for (const auto& interval : occupied) {
    if (cursor + candidateHeight <= interval.top - layoutGap + 0.5f) {
      return cursor;
    }
    cursor = std::max(cursor, interval.bottom + layoutGap);
  }

  if (cursor + candidateHeight <= bottom + 0.5f) {
    return cursor;
  }
  return std::nullopt;
}

uint32_t NotificationToast::surfaceHeightForOutput(wl_output* output) const {
  if (m_wayland != nullptr && output != nullptr) {
    if (const auto* wlOutput = m_wayland->findOutputByWl(output); wlOutput != nullptr) {
      const std::int32_t logicalHeight = outputLogicalHeight(*wlOutput);
      if (logicalHeight > 0) {
        const auto offsetY = m_config != nullptr
            ? static_cast<std::int32_t>(std::max(0, m_config->config().notification.offsetY))
            : std::int32_t{8};
        const std::int32_t available = logicalHeight - (offsetY * 2);
        return static_cast<uint32_t>(std::max(1, available));
      }
    }
  }

  return fallbackSurfaceHeight(notificationUiScale(m_config));
}

// --- Surface lifecycle ---

void NotificationToast::ensureSurfaces() {
  if (m_wayland == nullptr || m_renderContext == nullptr || m_config == nullptr) {
    return;
  }

  const float scale = notificationUiScale(m_config);
  const std::string position = notificationPosition();
  const std::string layer = notificationLayer();
  const auto selectedMonitors = notificationMonitors();
  const auto& notifCfg = m_config->config().notification;
  const int offX = std::max(0, notifCfg.offsetX);
  const int offY = std::max(0, notifCfg.offsetY);
  const auto surfaceWidth = ::surfaceWidth(scale, horizontalInnerPad(scale));
  const std::uint32_t anchor = toastSurfaceAnchor(position);
  const ToastSurfaceMargins margins = toastSurfaceMargins(position, offX, offY, scale);
  if (!m_instances.empty()
      && (position != m_lastPosition || layer != m_lastLayer || selectedMonitors != m_lastMonitorSelectors)) {
    for (auto& inst : m_instances) {
      inst->animations.cancelAll();
      inst->inputDispatcher.setSceneRoot(nullptr);
    }
    m_instances.clear();
  }
  m_lastPosition = position;
  m_lastLayer = layer;
  m_lastMonitorSelectors = selectedMonitors;

  // Monitor selection is a filter, not a binding: if none of the configured monitors are
  // currently connected (e.g. an external display was undocked), fall back to every available
  // output so notifications never silently disappear. Targeting is restored automatically when a
  // configured monitor reconnects via onOutputChange().
  const bool anyConfiguredPresent =
      selectedMonitors.empty()
      || std::any_of(m_wayland->outputs().begin(), m_wayland->outputs().end(), [this](const WaylandOutput& o) {
           return o.done && o.output != nullptr && o.hasUsableGeometry() && shouldRenderOnOutput(o);
         });

  std::erase_if(m_instances, [this, anyConfiguredPresent](const auto& inst) {
    if (inst == nullptr || inst->output == nullptr) {
      return true;
    }
    const auto* output = m_wayland->findOutputByWl(inst->output);
    if (output == nullptr || !output->done || !output->hasUsableGeometry()) {
      return true;
    }
    return anyConfiguredPresent && !shouldRenderOnOutput(*output);
  });

  for (const auto& output : m_wayland->outputs()) {
    if (!output.done || output.output == nullptr || !output.hasUsableGeometry()) {
      continue;
    }
    if (anyConfiguredPresent && !shouldRenderOnOutput(output)) {
      continue;
    }

    auto existingIt = std::ranges::find_if(m_instances, [&output](const auto& inst) {
      return inst != nullptr && inst->output == output.output;
    });
    if (existingIt != m_instances.end()) {
      auto& inst = *existingIt;
      if (inst->surface != nullptr) {
        if (inst->surface->marginTop() != margins.top
            || inst->surface->marginRight() != margins.right
            || inst->surface->marginBottom() != margins.bottom
            || inst->surface->marginLeft() != margins.left) {
          inst->surface->setMargins(margins.top, margins.right, margins.bottom, margins.left);
        }
        if (inst->surface->width() != surfaceWidth) {
          // Height 0 is only valid with top+bottom anchors; preserve current height otherwise.
          inst->surface->requestSize(surfaceWidth, 0);
        }
      }
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;

    auto surfaceConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-notification",
        .layer = layer == "overlay" ? LayerShellLayer::Overlay : LayerShellLayer::Top,
        .anchor = anchor,
        .width = surfaceWidth,
        .height = 0,
        .exclusiveZone = 0,
        .marginTop = margins.top,
        .marginRight = margins.right,
        .marginBottom = margins.bottom,
        .marginLeft = margins.left,
        .keyboard = LayerShellKeyboard::None,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeightForOutput(output.output),
        .prewarmBlur = true,
    };

    inst->surface = std::make_unique<LayerSurface>(*m_wayland, std::move(surfaceConfig));
    inst->surface->setRenderContext(m_renderContext);

    auto* instPtr = inst.get();
    inst->surface->setConfigureCallback([instPtr](uint32_t /*w*/, uint32_t /*h*/) {
      instPtr->surface->requestLayout();
    });
    inst->surface->setPrepareFrameCallback([this, instPtr](bool needsUpdate, bool needsLayout) {
      prepareFrame(*instPtr, needsUpdate, needsLayout);
    });
    inst->surface->setFrameTickCallback([this, instPtr](float /*deltaMs*/) {
      // Cards animate horizontally during entry/exit slides; the input and blur regions
      // must follow the visible position or the rounded right edge bleeds.
      if (instPtr->animations.hasActive()) {
        updateInputRegion(*instPtr);
      }
    });
    inst->surface->setAnimationManager(&inst->animations);

    bool ok = inst->surface->initialize(output.output);
    if (!ok) {
      kLog.warn("notification toast: failed to initialize surface on {}", output.connectorName);
      continue;
    }

    kLog.debug("notification toast: surface created on {}", output.connectorName);
    m_instances.push_back(std::move(inst));
  }
}

void NotificationToast::destroySurfaces() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->clearBlurRegion();
    }
    inst->animations.cancelAll();
    inst->inputDispatcher.setSceneRoot(nullptr);
  }
  m_instances.clear();
  m_entries.clear();
  kLog.debug("notification toast: all surfaces destroyed");
}

void NotificationToast::prepareFrame(Instance& inst, bool /*needsUpdate*/, bool needsLayout) {
  if (m_renderContext == nullptr || inst.surface == nullptr) {
    return;
  }

  const auto width = inst.surface->width();
  const auto height = inst.surface->height();
  if (width == 0 || height == 0) {
    return;
  }

  m_renderContext->makeCurrent(inst.surface->renderTarget());

  const bool needsSceneBuild = inst.sceneRoot == nullptr
      || static_cast<uint32_t>(std::round(inst.sceneRoot->width())) != width
      || static_cast<uint32_t>(std::round(inst.sceneRoot->height())) != height;
  const bool needsRebuild = needsSceneBuild || inst.rebuildRequested;
  inst.rebuildRequested = false;

  // Generic scene graph layout dirt can come from paint-only toast interactions,
  // such as reveal clipping or hover state. Rebuilding here would restart active
  // entry/exit/countdown animations; only explicit toast rebuild requests should
  // tear down the card scene.
  if (needsRebuild) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    alignBottomStackToPlacementBottom();
    buildScene(inst, width, height);
  } else if (needsLayout && inst.sceneRoot != nullptr) {
    // Control layout dirt (e.g. inline-reply Input caret/text metrics) must run here;
    // redraw-only leaves placeholder styling and a stuck caret at byte 0.
    UiPhaseScope layoutPhase(UiPhase::Layout);
    inst.sceneRoot->layout(*m_renderContext);
    inst.surface->requestRedraw();
  }
}

void NotificationToast::buildScene(Instance& inst, uint32_t width, uint32_t height) {
  uiAssertNotRendering("NotificationToast::buildScene");
  if (m_renderContext == nullptr) {
    return;
  }

  auto w = static_cast<float>(width);
  auto h = static_cast<float>(height);

  auto sceneRoot = ui::node({
      .width = w,
      .height = h,
      .configure = [&inst](Node& node) { node.setAnimationManager(&inst.animations); },
  });

  inst.inputDispatcher.setSceneRoot(sceneRoot.get());
  inst.inputDispatcher.setTextInputContext(inst.surface->wlSurface(), m_wayland->textInputService());
  inst.sceneRoot = std::move(sceneRoot);
  inst.inputDispatcher.setCursorShapeCallback([this](uint32_t serial, uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });

  inst.surface->setSceneRoot(inst.sceneRoot.get());

  // Build cards for any entries that already exist
  inst.cards.clear();
  inst.cards.resize(m_entries.size());
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (!m_entries[i].exiting
        && hasPlacement(m_entries[i])
        && fitsOnSurface(m_entries[i], static_cast<float>(height))) {
      addCardToInstance(inst, i);
    }
  }

  updateInputRegion(inst);
  if (inst.pointerInside) {
    inst.inputDispatcher.pointerMotion(inst.lastPointerX, inst.lastPointerY, 0);
  }

  std::vector<uint32_t> exitingIds;
  for (const auto& entry : m_entries) {
    if (entry.exiting) {
      exitingIds.push_back(entry.notificationId);
    }
  }
  if (!exitingIds.empty()) {
    DeferredCall::callLater([this, exitingIds = std::move(exitingIds)]() {
      for (const uint32_t id : exitingIds) {
        finishExitingEntryIfOrphaned(id);
      }
    });
  }
}

void NotificationToast::updateInputRegion(Instance& inst) const {
  if (inst.surface == nullptr) {
    return;
  }

  std::vector<InputRect> rects;
  std::vector<InputRect> blurRects;
  rects.reserve(inst.cards.size());
  for (const auto& card : inst.cards) {
    if (card.cardNode == nullptr) {
      continue;
    }
    if (card.cardNode->width() <= 0.5f || card.cardNode->height() <= 0.5f) {
      continue;
    }
    const int rx = static_cast<int>(std::floor(card.cardNode->x()));
    const int ry = static_cast<int>(std::floor(card.cardNode->y()));
    const int rw = std::max(1, static_cast<int>(std::ceil(card.cardNode->width())));
    const int rh = std::max(1, static_cast<int>(std::ceil(card.cardNode->height())));
    rects.push_back({rx, ry, rw, rh});
    auto strips = Surface::tessellateRoundedRect(rx, ry, rw, rh, Style::scaledRadiusXl(notificationUiScale(m_config)));
    blurRects.insert(blurRects.end(), strips.begin(), strips.end());
  }

  inst.surface->setInputRegion(rects);
  if (blurRects.empty()) {
    inst.surface->clearBlurRegion();
  } else {
    inst.surface->setBlurRegion(blurRects);
  }
}

float NotificationToast::cardReveal(const Instance::CardState& cs, float cardHeight) const {
  return cardRevealFromNode(cs.cardNode, revealDirection(), cardHeight, notificationUiScale(m_config));
}

void NotificationToast::applyCardReveal(Instance::CardState& cs, float reveal, float y, float cardHeight) const {
  const float scale = notificationUiScale(m_config);
  applyCardRevealNodes(
      cs.cardNode, cs.cardContent, cs.cardForeground, reveal, y, revealDirection(), cardHeight, scale,
      horizontalInnerPad(scale)
  );
}

InputArea* NotificationToast::buildCard(
    const PopupEntry& entry, Node** outCardContent, Node** outCardForeground, ProgressBar** outProgress,
    Node** outActionsRow, Node** outInlineReplyRow, Input** outInlineReplyInput
) {
  const float scale = notificationUiScale(m_config);
  const bool hasInlineReply = hasInlineReplyAction(entry.actions);
  const bool showActions = shouldShowNotificationActions(m_config);
  const float iconSize = notificationIconSize(scale, showActions);
  const float iconGlyphSize = notificationIconGlyphSize(scale, showActions);
  const float cardW = cardWidth(scale);
  const float maxCardHeight = maxToastCardHeight(scale);
  const float textMaxWidth = notificationTextMaxWidth(scale, showActions);
  const float topTextMaxWidth = std::max(0.0f, textMaxWidth - closeButtonSize(scale) - Style::spaceSm * scale);
  const bool showAppName = shouldShowNotificationAppName(m_config, entry.appName);

  auto viewport = std::make_unique<InputArea>();
  viewport->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  viewport->setOnClick([this, id = entry.notificationId,
                        hasDefaultAction = !entry.actions.empty()
                            && entry.actions.size() >= 2
                            && entry.actions[0] == "default"](const InputArea::PointerData& data) {
    if (data.button == BTN_RIGHT) {
      requestClose(id, CloseReason::Dismissed);
    } else if (data.button == BTN_LEFT && hasDefaultAction) {
      if (m_notifications != nullptr) {
        if (!m_notifications->invokeAction(id, "default", true)) {
          kLog.warn("notification toast: failed to invoke default action for #{}", id);
        }
      }
    }
  });
  viewport->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);

  auto cardRoot = ui::node({.out = outCardContent});

  auto foreground = ui::column({
      .maxHeight = maxCardHeight,
      .clipChildren = true,
      .width = cardW,
  });
  *outCardForeground = foreground.get();

  const float bgAlpha = m_config != nullptr ? m_config->config().notification.backgroundOpacity : 0.97f;
  const float borderWidth = (m_config == nullptr || m_config->config().notification.border) ? Style::borderWidth : 0.0f;
  foreground->addChild(
      ui::progressBar({
          .out = outProgress,
          .fill = toastProgressFillColor(entry.urgency),
          .track = clearColorSpec(),
          .radius = progressHeight(scale) * 0.5f,
          .orientation = ProgressBarOrientation::HorizontalCentered,
          .width = std::max(0.0f, cardW - topProgressInset(scale) * 2.0f),
          .height = progressHeight(scale),
          .visible = showToastProgressAccent(entry.urgency, entry.displayDurationMs),
          .participatesInLayout = showToastProgressAccent(entry.urgency, entry.displayDurationMs),
          .configure = [scale](ProgressBar& progress) { progress.setPosition(topProgressInset(scale), 0.0f); },
      })
  );

  auto contentRow = ui::row({
      .align = FlexAlign::Start,
      .gap = iconTextGap(scale),
      .padding = cardInnerPad(scale),
      .width = cardW,
  });

  auto iconSlot = ui::node({
      .width = iconSize,
      .height = iconSize,
  });

  bool iconAssigned = false;
  if (entry.icon.has_value()) {
    const std::string& rawIcon = *entry.icon;
    if (rawIcon.size() > kNoctaliaGlyphIconPrefix.size()
        && std::string_view(rawIcon.data(), kNoctaliaGlyphIconPrefix.size()) == kNoctaliaGlyphIconPrefix) {
      const std::string_view glyphName(
          rawIcon.data() + kNoctaliaGlyphIconPrefix.size(), rawIcon.size() - kNoctaliaGlyphIconPrefix.size()
      );
      if (!glyphName.empty()) {
        iconSlot->addChild(
            ui::glyph({
                .glyph = std::string(glyphName),
                .glyphSize = iconGlyphSize,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .configure = [this, iconSize](Glyph& glyph) {
                  glyph.measure(*m_renderContext);
                  glyph.setPosition(
                      std::round((iconSize - glyph.width()) * 0.5f), std::round((iconSize - glyph.height()) * 0.5f)
                  );
                },
            })
        );
        iconAssigned = true;
      }
    }
  }
  if (!iconAssigned) {
    if (entry.imageData.has_value()) {
      const auto& image = *entry.imageData;
      if (image.width > 0 && image.height > 0 && !image.data.empty()) {
        auto appIcon = ui::image({
            .fit = ImageFit::Cover,
            .radius = notificationIconRadius(iconSize, scale),
            .width = iconSize,
            .height = iconSize,
            .configure = [](Image& control) { control.setPosition(0.0f, 0.0f); },
        });
        const bool validImageMetadata = image.bitsPerSample == 8
            && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
        const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
        if (validImageMetadata
            && appIcon->setSourceRaw(
                *m_renderContext, image.data.data(), image.data.size(), image.width, image.height, image.rowStride,
                format, true
            )) {
          iconSlot->addChild(std::move(appIcon));
          iconAssigned = true;
        } else if (!validImageMetadata) {
          kLog.warn(
              "notification toast: unsupported image-data avatar metadata for #{} (alpha={}, bits={}, channels={})",
              entry.notificationId, image.hasAlpha, image.bitsPerSample, image.channels
          );
        } else {
          kLog.warn(
              "notification toast: failed to load image-data avatar for #{} ({}x{}, bytes={})", entry.notificationId,
              image.width, image.height, image.data.size()
          );
        }
      } else {
        kLog.warn(
            "notification toast: invalid image-data avatar for #{} ({}x{}, bytes={})", entry.notificationId,
            image.width, image.height, image.data.size()
        );
      }
    }

    if (!iconAssigned) {
      const std::string iconPath = resolveNotificationIconPath(entry);
      if (!iconPath.empty()) {
        auto appIcon = ui::image({
            .fit = ImageFit::Cover,
            .radius = notificationIconRadius(iconSize, scale),
            .width = iconSize,
            .height = iconSize,
            .configure = [](Image& image) { image.setPosition(0.0f, 0.0f); },
        });
        if (appIcon->setSourceFile(*m_renderContext, iconPath, static_cast<int>(std::round(iconSize)))) {
          iconSlot->addChild(std::move(appIcon));
          iconAssigned = true;
        } else {
          kLog.warn("notification toast: failed to load icon image for #{} from '{}'", entry.notificationId, iconPath);
        }
      }
    }
  }

  if (!iconAssigned) {
    iconSlot->addChild(
        ui::glyph({
            .glyph = "bell",
            .glyphSize = iconGlyphSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .configure = [this, iconSize](Glyph& glyph) {
              glyph.measure(*m_renderContext);
              glyph.setPosition(
                  std::round((iconSize - glyph.width()) * 0.5f), std::round((iconSize - glyph.height()) * 0.5f)
              );
            },
        })
    );
  }

  contentRow->addChild(std::move(iconSlot));

  const std::string displaySummary = StringUtils::trimLeadingBlankLines(entry.summary);
  const std::string displayBody = StringUtils::trimLeadingBlankLines(entry.body);
  auto textColumn = ui::column(
      {
          .align = FlexAlign::Stretch,
          .width = textMaxWidth,
      },
      ui::label({
          .text = displaySummary,
          .fontSize = summaryFontSize(scale),
          .fontWeight = FontWeight::Bold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = topTextMaxWidth,
          .maxLines = kMaxSummaryLines,
      })
  );
  std::unique_ptr<Flex> actionsRow;
  std::unique_ptr<Flex> inlineReplyRow;
  std::unique_ptr<Input> inlineReplyInput;
  std::unique_ptr<Button> inlineReplySendButton;
  Input* inlineReplyInputPtr = nullptr;
  if (showActions && !entry.actions.empty()) {
    const uint32_t notificationId = entry.notificationId;
    const int totalDuration = entry.displayDurationMs;

    // Build action buttons row (always visible initially)
    {
      std::vector<std::unique_ptr<Button>> buttons;
      const std::size_t limit = std::min(entry.actions.size(), kMaxNotificationActions * 2);
      for (std::size_t i = 0; i + 1 < limit; i += 2) {
        const std::string actionKey = entry.actions[i];
        std::string actionLabel = entry.actions[i + 1];
        if (actionKey.empty() || actionKey == "default") {
          continue;
        }
        if (StringUtils::isBlank(actionLabel)) {
          actionLabel = fallbackActionLabel();
        }

        auto actionButton = makeNotificationActionButton(actionLabel, scale);
        actionButton->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
        actionButton->setOnEnter([this, notificationId]() { beginPopupHover(notificationId); });
        actionButton->setOnLeave([this, notificationId, totalDuration]() {
          endPopupHover(notificationId, totalDuration);
        });
        actionButton->setOnClick([this, id = entry.notificationId, actionKey]() {
          if (actionKey == "inline-reply") {
            enterInlineReplyMode(id);
            return;
          }
          if (m_notifications == nullptr) {
            return;
          }
          if (!m_notifications->invokeAction(id, actionKey, true)) {
            kLog.warn("notification toast: failed to invoke action '{}' for #{}", actionKey, id);
          }
        });
        buttons.push_back(std::move(actionButton));
      }

      if (!buttons.empty()) {
        actionsRow = ui::column({
            .padding = Style::spaceXs * scale,
        });
        layoutNotificationActionsRow(*m_renderContext, *actionsRow, buttons, scale);
      }
    }

    // Inline reply row (hidden until the user taps Reply).
    if (hasInlineReply) {
      const uint32_t replyNotificationId = entry.notificationId;
      const int replyTotalDuration = entry.displayDurationMs;
      inlineReplyRow = ui::row({
          .align = FlexAlign::Center,
          .gap = kInlineReplyGap * scale,
          .padding = Style::spaceXs * scale,
          .width = textMaxWidth,
          .visible = false,
          .participatesInLayout = false,
      });

      inlineReplyInput = ui::input({
          .out = &inlineReplyInputPtr,
          .placeholder = inlineReplyPlaceholder(entry.actions),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = kInlineReplyInputHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .frameVisible = true,
          .flexGrow = 1.0f,
          .onSubmit = [this, id = entry.notificationId](const std::string& text) { submitInlineReply(id, text); },
          .configure =
              [this, replyNotificationId, replyTotalDuration](Input& input) {
                InputArea* const replyInputArea = input.inputArea();
                replyInputArea->setOnFocusGain([this, replyNotificationId]() {
                  if (auto* popup = findEntry(replyNotificationId); popup != nullptr) {
                    popup->replyInputFocused = true;
                    for (auto& inst : m_instances) {
                      if (auto* state = findCardState(*inst, replyNotificationId);
                          state != nullptr && state->progressBar != nullptr) {
                        popup->remainingProgress = std::clamp(state->progressBar->progress(), 0.0f, 1.0f);
                        break;
                      }
                    }
                  }
                  pauseCountdowns(replyNotificationId);
                  if (m_notifications != nullptr) {
                    m_notifications->pauseExpiry(replyNotificationId);
                  }
                });
                replyInputArea->setOnFocusLoss([this, replyNotificationId, replyTotalDuration]() {
                  if (auto* popup = findEntry(replyNotificationId); popup != nullptr) {
                    for (auto& inst : m_instances) {
                      if (auto* state = findCardState(*inst, replyNotificationId);
                          state != nullptr && state->progressBar != nullptr) {
                        popup->remainingProgress = std::clamp(state->progressBar->progress(), 0.0f, 1.0f);
                        break;
                      }
                    }
                    popup->replyInputFocused = false;
                  }
                  resumeTimeout(replyNotificationId, replyTotalDuration);
                });
                // Pointer hover must not restyle the field; only real focus should show the active chrome.
                replyInputArea->setOnEnter([this, replyNotificationId](const InputArea::PointerData&) {
                  beginPopupHover(replyNotificationId);
                });
                replyInputArea->setOnLeave([this, replyNotificationId, replyTotalDuration]() {
                  endPopupHover(replyNotificationId, replyTotalDuration);
                });
              },
      });

      inlineReplySendButton = ui::button({
          .glyph = "send",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Default,
          .minWidth = kInlineReplySendButtonSize * scale,
          .minHeight = kInlineReplySendButtonSize * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .onClick = [this, id = entry.notificationId]() { submitInlineReply(id, {}); },
          .onEnter = [this, notificationId = entry.notificationId]() { beginPopupHover(notificationId); },
          .onLeave = [this, notificationId = entry.notificationId,
                      totalDuration = entry.displayDurationMs]() { endPopupHover(notificationId, totalDuration); },
          .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER); },
      });

      inlineReplyRow->addChild(std::move(inlineReplyInput));
      inlineReplyRow->addChild(std::move(inlineReplySendButton));
      inlineReplyInput = nullptr;
    }
  }

  auto body = ui::label({
      .text = displayBody,
      .fontSize = bodyFontSize(scale),
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxWidth = textMaxWidth,
      .maxLines = std::max(1, entry.toastBodyLines),
      .visible = entry.toastBodyLines > 0 && !StringUtils::isBlank(displayBody),
      .participatesInLayout = entry.toastBodyLines > 0 && !StringUtils::isBlank(displayBody),
  });
  textColumn->addChild(std::move(body));

  if (actionsRow != nullptr) {
    *outActionsRow = actionsRow.get();
    textColumn->addChild(std::move(actionsRow));
  } else {
    *outActionsRow = nullptr;
  }

  if (inlineReplyRow != nullptr) {
    *outInlineReplyRow = inlineReplyRow.get();
    *outInlineReplyInput = inlineReplyInputPtr;
    textColumn->addChild(std::move(inlineReplyRow));
  } else {
    *outInlineReplyRow = nullptr;
    *outInlineReplyInput = nullptr;
  }

  if (showAppName) {
    auto footerRow = ui::row(
        {
            .align = FlexAlign::Center,
            .justify = FlexJustify::End,
            .padding = Style::spaceXs * scale,
            .width = textMaxWidth,
        },
        ui::label({
            .text = entry.appName,
            .fontSize = metaFontSize(scale),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = textMaxWidth,
            .maxLines = 1,
            .textAlign = TextAlign::End,
        })
    );
    textColumn->addChild(std::move(footerRow));
  }

  contentRow->addChild(std::move(textColumn));
  foreground->addChild(std::move(contentRow));
  foreground->layout(*m_renderContext);

  const float cardHeight = std::min(maxCardHeight, foreground->height());
  cardRoot->setSize(cardW, cardHeight);
  viewport->setSize(cardW, cardHeight);
  viewport->setClipChildren(true);

  cardRoot->addChild(
      ui::box({
          .width = cardW,
          .height = cardHeight,
          .configure = [scale, bgAlpha, borderWidth](Box& box) {
            box.setCardStyle();
            box.setRadius(Style::scaledRadiusXl(scale));
            box.setFill(colorSpecFromRole(ColorRole::Surface, bgAlpha));
            box.setBorder(colorSpecFromRole(ColorRole::Outline), borderWidth);
          },
      })
  );

  cardRoot->addChild(std::move(foreground));
  cardRoot->addChild(
      ui::button({
          .glyph = "close",
          .glyphSize = kCloseGlyphSize * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = closeButtonSize(scale),
          .minHeight = closeButtonSize(scale),
          .padding = 0.0f,
          .onClick = [this, id = entry.notificationId]() { requestClose(id, CloseReason::Dismissed); },
          .onEnter = [this, notificationId = entry.notificationId]() { beginPopupHover(notificationId); },
          .onLeave = [this, notificationId = entry.notificationId,
                      totalDuration = entry.displayDurationMs]() { endPopupHover(notificationId, totalDuration); },
          .configure =
              [cardW, scale](Button& button) {
                button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
                button.setPosition(cardW - cardInnerPad(scale) - closeButtonSize(scale), cardInnerPad(scale));
              },
      })
  );
  cardRoot->layout(*m_renderContext);
  viewport->addChild(std::move(cardRoot));

  return viewport.release();
}

void NotificationToast::submitInlineReply(uint32_t notificationId, const std::string& replyText) {
  if (m_notifications == nullptr) {
    return;
  }

  std::string text = replyText;
  if (StringUtils::isBlank(text)) {
    for (auto& inst : m_instances) {
      if (auto* state = findCardState(*inst, notificationId); state != nullptr && state->inlineReplyInput != nullptr) {
        text = state->inlineReplyInput->value();
        break;
      }
    }
  }

  if (StringUtils::isBlank(text)) {
    return;
  }

  if (!m_notifications->invokeInlineReply(notificationId, text, true)) {
    kLog.warn("notification toast: failed to invoke inline-reply for #{}", notificationId);
  }
}

void NotificationToast::enterInlineReplyMode(uint32_t notificationId) {
  for (auto& inst : m_instances) {
    if (inst->sceneRoot == nullptr) {
      continue;
    }
    for (std::size_t ci = 0; ci < inst->cards.size() && ci < m_entries.size(); ++ci) {
      if (m_entries[ci].notificationId != notificationId) {
        continue;
      }
      auto& cs = inst->cards[ci];
      cs.replyMode = true;
      if (cs.actionsRowNode != nullptr) {
        cs.actionsRowNode->setVisible(false);
        cs.actionsRowNode->setParticipatesInLayout(false);
      }
      if (cs.inlineReplyRowNode != nullptr) {
        cs.inlineReplyRowNode->setVisible(true);
        cs.inlineReplyRowNode->setParticipatesInLayout(true);
      }
      if (inst->surface != nullptr) {
        inst->surface->requestLayout();
      }
    }
    syncKeyboardInteractivity(*inst);
  }
}

bool NotificationToast::isInlineReplyInputArea(const Instance& inst, const InputArea* area) {
  if (area == nullptr) {
    return false;
  }
  for (const auto& cs : inst.cards) {
    if (cs.inlineReplyInput != nullptr && cs.inlineReplyInput->inputArea() == area) {
      return true;
    }
  }
  return false;
}

void NotificationToast::clearInlineReplyFocus(Instance& inst) {
  if (!isInlineReplyInputArea(inst, inst.inputDispatcher.focusedArea())) {
    return;
  }
  inst.inputDispatcher.setFocus(nullptr);
  if (inst.surface != nullptr) {
    inst.surface->requestRedraw();
  }
}

bool NotificationToast::inputAreaBelongsToCard(const Instance::CardState& card, const InputArea* area) {
  if (area == nullptr || card.cardNode == nullptr) {
    return false;
  }
  for (const Node* node = area; node != nullptr; node = node->parent()) {
    if (node == card.cardNode) {
      return true;
    }
  }
  return false;
}

bool NotificationToast::pointerHitsInlineReplyInput(const Instance& inst, const Node* hit) {
  if (hit == nullptr) {
    return false;
  }
  for (const Node* node = hit; node != nullptr; node = node->parent()) {
    for (const auto& cs : inst.cards) {
      if (cs.inlineReplyInput == nullptr) {
        continue;
      }
      if (node == cs.inlineReplyInput || node == cs.inlineReplyInput->inputArea()) {
        return true;
      }
    }
  }
  return false;
}

void NotificationToast::syncKeyboardInteractivity(Instance& inst) const {
  if (inst.surface == nullptr) {
    return;
  }

  bool needsKeyboard = false;
  for (std::size_t ci = 0; ci < inst.cards.size() && ci < m_entries.size(); ++ci) {
    if (inst.cards[ci].replyMode) {
      needsKeyboard = true;
      break;
    }
  }

  inst.surface->setKeyboardInteractivity(needsKeyboard ? LayerShellKeyboard::OnDemand : LayerShellKeyboard::None);
}

bool NotificationToast::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_wayland == nullptr) {
    return false;
  }

  wl_surface* const kbSurface = m_wayland->lastKeyboardSurface();
  if (kbSurface == nullptr) {
    return false;
  }

  Instance* target = nullptr;
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr && inst->surface->wlSurface() == kbSurface) {
      target = inst.get();
      break;
    }
  }
  if (target == nullptr) {
    return false;
  }

  InputArea* const focused = target->inputDispatcher.focusedArea();
  if (focused == nullptr) {
    return false;
  }
  bool replyInputActive = false;
  for (std::size_t ci = 0; ci < target->cards.size() && ci < m_entries.size(); ++ci) {
    const auto& cs = target->cards[ci];
    if (cs.inlineReplyInput != nullptr && cs.inlineReplyInput->inputArea() == focused) {
      replyInputActive = true;
      break;
    }
  }
  if (!replyInputActive) {
    return false;
  }

  target->inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (target->sceneRoot != nullptr
      && (target->sceneRoot->paintDirty() || target->sceneRoot->layoutDirty())
      && target->surface != nullptr) {
    target->surface->requestRedraw();
  }
  return true;
}

// --- Pointer events ---

bool NotificationToast::onPointerEvent(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    for (auto& inst : m_instances) {
      if (inst->surface == nullptr) {
        continue;
      }
      if (event.surface != inst->surface->wlSurface()) {
        clearInlineReplyFocus(*inst);
        continue;
      }
      if (inst->sceneRoot != nullptr) {
        Node* const hit =
            Node::hitTest(inst->sceneRoot.get(), static_cast<float>(event.sx), static_cast<float>(event.sy));
        if (!pointerHitsInlineReplyInput(*inst, hit)) {
          clearInlineReplyFocus(*inst);
        }
      }
    }
  }

  bool consumed = false;

  for (auto& inst : m_instances) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (event.surface == inst->surface->wlSurface()) {
        inst->pointerInside = true;
        inst->lastPointerX = static_cast<float>(event.sx);
        inst->lastPointerY = static_cast<float>(event.sy);
        inst->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (event.surface == inst->surface->wlSurface()) {
        inst->pointerInside = false;
        resetInstanceHover(*inst, true);
        clearInlineReplyFocus(*inst);
        inst->inputDispatcher.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (inst->pointerInside) {
        inst->lastPointerX = static_cast<float>(event.sx);
        inst->lastPointerY = static_cast<float>(event.sy);
        inst->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
      }
      break;
    case PointerEvent::Type::Button:
      if (inst->pointerInside) {
        inst->lastPointerX = static_cast<float>(event.sx);
        inst->lastPointerY = static_cast<float>(event.sy);
        const bool pressed = event.pressed;
        inst->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
        inst->inputDispatcher.pointerButton(
            static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed
        );
        consumed = true;
      }
      break;
    case PointerEvent::Type::Axis:
      break;
    }

    // Hover state changes on child controls (buttons, close icons) can mark the tree
    // layoutDirty, but toast cards do not actually change geometry on hover — and
    // requestLayout() here would tear down the whole scene via buildScene(), killing
    // any in-flight entry/exit/slide/countdown animations. Treat pointer-driven dirt
    // as a redraw.
    if (inst->sceneRoot != nullptr && (inst->sceneRoot->paintDirty() || inst->sceneRoot->layoutDirty())) {
      inst->surface->requestRedraw();
    }
  }

  return consumed;
}

std::string NotificationToast::resolveNotificationIconPath(const PopupEntry& entry) {
  if (!entry.icon.has_value() || entry.icon->empty()) {
    return {};
  }
  if (entry.icon->size() > kNoctaliaGlyphIconPrefix.size()
      && std::string_view(entry.icon->data(), kNoctaliaGlyphIconPrefix.size()) == kNoctaliaGlyphIconPrefix) {
    return {};
  }

  const std::string iconValue = *entry.icon;

  if (isRemoteIconUrl(iconValue)) {
    if (const auto it = m_remoteIconCache.find(iconValue); it != m_remoteIconCache.end()) {
      std::error_code ec;
      if (std::filesystem::exists(it->second, ec) && std::filesystem::file_size(it->second, ec) > 0) {
        return it->second;
      }
      kLog.warn("notification toast: #{} remote cache entry stale path='{}'", entry.notificationId, it->second);
      m_remoteIconCache.erase(it);
    }

    if (m_failedRemoteIconDownloads.contains(iconValue)) {
      kLog.warn("notification toast: #{} remote icon URL marked failed url='{}'", entry.notificationId, iconValue);
      return {};
    }

    const auto cached = remoteIconCachePath(iconValue);
    std::error_code ec;
    if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
      const std::string cachedPath = cached.string();
      m_remoteIconCache[iconValue] = cachedPath;
      return cachedPath;
    }

    if (m_httpClient != nullptr && !m_pendingRemoteIconDownloads.contains(iconValue)) {
      std::filesystem::create_directories(cached.parent_path(), ec);
      if (ec) {
        kLog.warn(
            "notification toast: #{} failed to create icon cache dir '{}' error='{}'", entry.notificationId,
            cached.parent_path().string(), ec.message()
        );
      }

      m_pendingRemoteIconDownloads.insert(iconValue);
      m_httpClient->download(iconValue, cached, [this, url = iconValue, path = cached.string()](bool success) {
        m_pendingRemoteIconDownloads.erase(url);
        if (!success) {
          kLog.warn("notification toast: remote icon download failed url='{}'", url);
          m_failedRemoteIconDownloads.insert(url);
          return;
        }

        m_failedRemoteIconDownloads.erase(url);
        m_remoteIconCache[url] = path;
        requestLayout();
      });
    } else if (m_httpClient == nullptr) {
      kLog.warn("notification toast: cannot download remote icon url='{}' because HttpClient is null", iconValue);
    }
    return {};
  }

  const std::string localPath = normalizeLocalIconPath(iconValue);
  if (!localPath.empty() && localPath.front() == '/') {
    if (access(localPath.c_str(), R_OK) == 0) {
      return localPath;
    }
    kLog.warn("notification toast: #{} local icon path not readable path='{}'", entry.notificationId, localPath);
    return {};
  }

  if (localPath.empty()) {
    kLog.warn("notification toast: #{} icon value normalized to empty path", entry.notificationId);
    return {};
  }

  const std::string& resolved = m_iconResolver.resolve(
      localPath,
      static_cast<int>(
          std::round(notificationIconSize(notificationUiScale(m_config), shouldShowNotificationActions(m_config)))
      )
  );
  if (!resolved.empty()) {
    return resolved;
  }

  kLog.warn("notification toast: #{} theme icon not found name='{}'", entry.notificationId, localPath);
  return {};
}
