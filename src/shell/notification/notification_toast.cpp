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
#include "notification/notification_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr Logger kLog("notification");

  constexpr int kCardWidth = 360;
  constexpr int kCardHeightCompact = 132;
  constexpr int kCardHeightWithActions = 170;
  constexpr float kInlineReplyInputHeight = Style::controlHeightSm;
  constexpr float kInlineReplyGap = Style::spaceSm;
  constexpr float kInlineReplySendButtonSize = Style::controlHeightSm;

  constexpr float kGap = Style::spaceSm;
  constexpr float kPaddingX = Style::spaceMd;
  constexpr float kPaddingTop = 0.0f;
  constexpr float kPaddingBottom = Style::spaceMd;
  constexpr int kFallbackVisibleCards = 5;
  constexpr std::int32_t kHorizontalRevealPadding = static_cast<std::int32_t>(kPaddingX);
  constexpr float kQueuedY = -1.0f;
  constexpr float kCardInnerPad = Style::spaceMd;
  constexpr float kCloseButtonSize = 20.0f;
  constexpr float kCloseGlyphSize = 12.0f;
  constexpr float kNotificationIconSize = 42.0f;
  constexpr float kNotificationIconGlyphSize = 24.0f;
  constexpr float kNotificationIconReferenceSize = 36.0f;

  float notificationIconRadius(float iconSize, float localScale = 1.0f) {
    const float baseRadius = Style::radiusMd * (iconSize / kNotificationIconReferenceSize);
    return std::min(iconSize * 0.5f, Style::scaledRadius(baseRadius, localScale));
  }
  constexpr std::string_view kNoctaliaGlyphIconPrefix = "noctalia-glyph:";
  constexpr float kIconTextGap = Style::spaceSm;
  constexpr float kActionGap = Style::spaceXs;
  constexpr float kActionRowGap = Style::spaceSm;
  constexpr int kMaxActionButtons = 2;
  std::string fallbackActionLabel() { return i18n::tr("notifications.actions.fallback"); }

  bool hasInlineReplyAction(const std::vector<std::string>& actions) {
    for (std::size_t i = 0; i + 1 < actions.size(); i += 2) {
      if (actions[i] == "inline-reply") {
        return true;
      }
    }
    return false;
  }

  std::string inlineReplyPlaceholder(const std::vector<std::string>& actions) {
    for (std::size_t i = 0; i + 1 < actions.size(); i += 2) {
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
    return std::max(1000, static_cast<int>(timeout));
  }
  constexpr int kProgressHeight = 3;
  constexpr int kContentSlideOffset = 12;                 // subtle foreground slide during reveal/retract
  constexpr float kProgressBottomMargin = Style::spaceMd; // space below progress bar to card edge
  constexpr float kBodyBottomGap = Style::spaceSm;        // gap between body text and progress bar

  constexpr float kMetaFontSize = Style::fontSizeCaption;
  constexpr float kSummaryFontSize = Style::fontSizeTitle;
  constexpr float kBodyFontSize = Style::fontSizeBody;

  constexpr float kMetaGap = Style::spaceXs;        // vertical gap between app name and summary
  constexpr float kSummaryBodyGap = Style::spaceSm; // vertical gap between summary and body

  constexpr int kMaxSummaryLines = 2;
  constexpr int kToastMaxBodyLines = 3;
  constexpr int kMaxToastCardHeight = 320;

  [[nodiscard]] float notificationUiScale(const ConfigService* config) {
    if (config == nullptr) {
      return 1.0f;
    }
    const auto& shell = config->config().shell;
    const auto& notification = config->config().notification;
    return std::max(0.1f, shell.uiScale * notification.scale);
  }

  [[nodiscard]] float cardWidth(float scale) { return static_cast<float>(kCardWidth) * scale; }

  [[nodiscard]] float cardHeightForEntry(bool hasActions, float scale) {
    return (hasActions ? static_cast<float>(kCardHeightWithActions) : static_cast<float>(kCardHeightCompact)) * scale;
  }

  [[nodiscard]] float paddingTop(float scale) { return kPaddingTop * scale; }

  [[nodiscard]] float paddingX(float scale) { return kPaddingX * scale; }

  [[nodiscard]] float paddingBottom(float scale) { return kPaddingBottom * scale; }

  [[nodiscard]] float cardInnerPad(float scale) { return kCardInnerPad * scale; }

  [[nodiscard]] float closeButtonSize(float scale) { return kCloseButtonSize * scale; }

  [[nodiscard]] float notificationIconSize(float scale) { return kNotificationIconSize * scale; }

  [[nodiscard]] float iconTextGap(float scale) { return kIconTextGap * scale; }

  [[nodiscard]] float actionGap(float scale) { return kActionGap * scale; }

  [[nodiscard]] float actionRowGap(float scale) { return kActionRowGap * scale; }

  [[nodiscard]] float progressHeight(float scale) { return static_cast<float>(kProgressHeight) * scale; }

  [[nodiscard]] float progressBottomMargin(float scale) { return kProgressBottomMargin * scale; }

  [[nodiscard]] float bodyBottomGap(float scale) { return kBodyBottomGap * scale; }

  [[nodiscard]] float metaFontSize(float scale) { return kMetaFontSize * scale; }

  [[nodiscard]] float summaryFontSize(float scale) { return kSummaryFontSize * scale; }

  [[nodiscard]] float bodyFontSize(float scale) { return kBodyFontSize * scale; }

  [[nodiscard]] float metaGap(float scale) { return kMetaGap * scale; }

  [[nodiscard]] float summaryBodyGap(float scale) { return kSummaryBodyGap * scale; }

  [[nodiscard]] float maxToastCardHeight(float scale) { return static_cast<float>(kMaxToastCardHeight) * scale; }

  [[nodiscard]] std::uint32_t surfaceWidth(float scale) {
    return static_cast<std::uint32_t>(
        std::max(1, static_cast<int>(std::ceil(cardWidth(scale) + paddingX(scale) * 2.0f)))
    );
  }

  [[nodiscard]] std::uint32_t fallbackSurfaceHeight(float scale) {
    const float totalHeight = maxToastCardHeight(scale) * kFallbackVisibleCards +
                              (kGap * scale) * (kFallbackVisibleCards - 1) + paddingBottom(scale);
    return static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::ceil(totalHeight))));
  }

  [[nodiscard]] std::int32_t horizontalRevealPadding(float scale) {
    return static_cast<std::int32_t>(std::lround(paddingX(scale)));
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
      NotificationToast::RevealDirection direction, float cardHeight, float scale
  ) {
    if (cardNode == nullptr || cardContent == nullptr || cardForeground == nullptr) {
      return;
    }

    const float clampedReveal = std::clamp(reveal, 0.0f, 1.0f);
    const float contentSlide = contentOffsetForReveal(clampedReveal, scale);

    switch (direction) {
    case NotificationToast::RevealDirection::FromLeft: {
      const float visibleWidth = std::round(cardWidth(scale) * clampedReveal);
      cardNode->setPosition(paddingX(scale), y);
      cardNode->setFrameSize(visibleWidth, cardHeight);
      cardContent->setPosition(0.0f, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(-contentSlide, 0.0f);
      break;
    }
    case NotificationToast::RevealDirection::FromRight: {
      const float visibleWidth = std::round(cardWidth(scale) * clampedReveal);
      const float hiddenWidth = cardWidth(scale) - visibleWidth;
      cardNode->setPosition(paddingX(scale) + hiddenWidth, y);
      cardNode->setFrameSize(visibleWidth, cardHeight);
      cardContent->setPosition(-hiddenWidth, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(contentSlide, 0.0f);
      break;
    }
    case NotificationToast::RevealDirection::FromTop: {
      const float visibleHeight = std::round(cardHeight * clampedReveal);
      cardNode->setPosition(paddingX(scale), y);
      cardNode->setFrameSize(cardWidth(scale), visibleHeight);
      cardContent->setPosition(0.0f, 0.0f);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(0.0f, -contentSlide);
      break;
    }
    case NotificationToast::RevealDirection::FromBottom: {
      const float visibleHeight = std::round(cardHeight * clampedReveal);
      const float hiddenHeight = cardHeight - visibleHeight;
      cardNode->setPosition(paddingX(scale), y + hiddenHeight);
      cardNode->setFrameSize(cardWidth(scale), visibleHeight);
      cardContent->setPosition(0.0f, -hiddenHeight);
      cardForeground->setOpacity(contentOpacityForReveal(clampedReveal));
      cardForeground->setPosition(0.0f, contentSlide);
      break;
    }
    }
  }

  std::int32_t outputLogicalHeight(const WaylandOutput& output) {
    if (output.logicalHeight > 0) {
      return output.logicalHeight;
    }
    if (output.height > 0) {
      return output.height / std::max(1, output.scale);
    }
    return 0;
  }

  float bodyTopForSummary(float summaryHeight, float scale) {
    return cardInnerPad(scale) + closeButtonSize(scale) + metaGap(scale) + summaryHeight + summaryBodyGap(scale);
  }

  float availableBodyHeight(float summaryHeight, float actionsReservedHeight, float cardHeight, float scale) {
    const float progressY = cardHeight - progressHeight(scale) - progressBottomMargin(scale);
    const float availableHeight =
        progressY - bodyBottomGap(scale) - actionsReservedHeight - bodyTopForSummary(summaryHeight, scale);
    return availableHeight;
  }

  float notificationTextStartX(float scale) {
    return cardInnerPad(scale) + notificationIconSize(scale) + iconTextGap(scale);
  }

  float notificationTextMaxWidth(float scale) {
    return std::max(0.0f, cardWidth(scale) - notificationTextStartX(scale) - cardInnerPad(scale));
  }

  std::unique_ptr<Button> makeNotificationActionButton(std::string_view label, float scale) {
    return ui::button({
        .text = std::string(label),
        .fontSize = Style::fontSizeCaption * scale,
        .variant = ButtonVariant::Outline,
    });
  }

  std::vector<std::unique_ptr<Button>>
  collectNotificationActionButtons(const std::vector<std::string>& actions, float scale) {
    std::vector<std::unique_ptr<Button>> buttons;
    buttons.reserve(kMaxActionButtons);
    for (std::size_t i = 0; i + 1 < actions.size() && static_cast<int>(buttons.size()) < kMaxActionButtons; i += 2) {
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

  bool
  notificationActionsPreferStack(RenderContext& rc, const std::vector<std::unique_ptr<Button>>& buttons, float scale) {
    if (buttons.size() < 2) {
      return false;
    }
    const float rowWidth = notificationTextMaxWidth(scale);
    float totalWidth = 0.0f;
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (i > 0) {
        totalWidth += actionGap(scale);
      }
      const LayoutSize measured = buttons[i]->measure(rc, LayoutConstraints{});
      totalWidth += measured.width;
    }
    return totalWidth > rowWidth + 0.5f;
  }

  void configureNotificationActionsRow(Flex& row, bool stacked, float scale) {
    if (stacked) {
      row.setDirection(FlexDirection::Vertical);
      row.setAlign(FlexAlign::Start);
      row.setJustify(FlexJustify::End);
    } else {
      row.setDirection(FlexDirection::Horizontal);
      row.setAlign(FlexAlign::Center);
      row.setJustify(FlexJustify::Start);
    }
    row.setGap(actionGap(scale));
  }

  float layoutNotificationActionsRow(
      RenderContext& rc, Flex& row, std::vector<std::unique_ptr<Button>>& buttons, float scale
  ) {
    const bool stacked = notificationActionsPreferStack(rc, buttons, scale);
    configureNotificationActionsRow(row, stacked, scale);
    const float rowWidth = notificationTextMaxWidth(scale);
    for (auto& button : buttons) {
      if (stacked) {
        button->setMaxWidth(0.0f);
      } else if (buttons.size() == 1) {
        button->setMaxWidth(rowWidth);
      } else {
        button->setMaxWidth(0.0f);
      }
      row.addChild(std::move(button));
    }
    buttons.clear();
    row.setSize(rowWidth, 0.0f);
    row.layout(rc);
    return row.height() + actionRowGap(scale);
  }

  bool isCloseButtonHit(float localX, float localY, float scale) {
    const float closeLeft = cardWidth(scale) - cardInnerPad(scale) - closeButtonSize(scale);
    const float closeTop = cardInnerPad(scale);
    return localX >= closeLeft && localX < closeLeft + closeButtonSize(scale) && localY >= closeTop &&
           localY < closeTop + closeButtonSize(scale);
  }

  float measureActionsFromPairs(RenderContext& rc, const std::vector<std::string>& actions, float scale) {
    auto buttons = collectNotificationActionButtons(actions, scale);
    if (buttons.empty()) {
      return 0.0f;
    }
    auto actionsRow = ui::makeFlex(FlexDirection::Horizontal, {});
    return layoutNotificationActionsRow(rc, *actionsRow, buttons, scale);
  }

  struct ToastGeometry {
    int summaryLines = kMaxSummaryLines;
    int bodyLines = 0;
    float summaryHeightPx = 0.0f;
    float cardHeight = 0.0f;
  };

  float requiredToastCardHeight(float summaryHeight, float bodyHeight, float actionsReserved, float scale) {
    const float bodyTop = bodyTopForSummary(summaryHeight, scale);
    return bodyTop + bodyHeight + bodyBottomGap(scale) + actionsReserved + progressHeight(scale) +
           progressBottomMargin(scale);
  }

  ToastGeometry planToastLayout(
      RenderContext& rc, std::string_view summary, std::string_view body, const std::vector<std::string>& actions,
      float floorCardHeight, float scale
  ) {
    const std::string displaySummary = StringUtils::trimLeadingBlankLines(summary);
    const std::string displayBody = StringUtils::trimLeadingBlankLines(body);
    const float textMaxWidth = notificationTextMaxWidth(scale);
    const float actionsReserved = measureActionsFromPairs(rc, actions, scale);
    const float maxCard = maxToastCardHeight(scale);
    const float floorH = floorCardHeight;

    ToastGeometry out;

    if (StringUtils::isBlank(displayBody)) {
      Label summaryProbe;
      summaryProbe.setFontSize(summaryFontSize(scale));
      summaryProbe.setFontWeight(FontWeight::Bold);
      summaryProbe.setMaxWidth(textMaxWidth);
      summaryProbe.setText(displaySummary);
      summaryProbe.setMaxLines(kMaxSummaryLines);
      summaryProbe.measure(rc);
      const float sumH = summaryProbe.height();
      const float required = requiredToastCardHeight(sumH, 0.0f, actionsReserved, scale);
      out.summaryLines = kMaxSummaryLines;
      out.bodyLines = 0;
      out.summaryHeightPx = sumH;
      out.cardHeight = std::min(maxCard, std::max(floorH, std::ceil(required)));
      return out;
    }

    static constexpr std::array<std::pair<int, int>, 6> kPreference = {{
        {2, kToastMaxBodyLines},
        {2, 2},
        {2, 1},
        {1, kToastMaxBodyLines},
        {1, 2},
        {1, 1},
    }};

    for (const auto& [sl, bl] : kPreference) {
      Label summaryProbe;
      summaryProbe.setFontSize(summaryFontSize(scale));
      summaryProbe.setFontWeight(FontWeight::Bold);
      summaryProbe.setMaxWidth(textMaxWidth);
      summaryProbe.setText(displaySummary);
      summaryProbe.setMaxLines(sl);
      summaryProbe.measure(rc);
      const float sumH = summaryProbe.height();

      Label bodyProbe;
      bodyProbe.setFontSize(bodyFontSize(scale));
      bodyProbe.setMaxWidth(textMaxWidth);
      bodyProbe.setText(displayBody);
      bodyProbe.setMaxLines(bl);
      bodyProbe.measure(rc);
      const float bodyH = bodyProbe.height();

      const float required = requiredToastCardHeight(sumH, bodyH, actionsReserved, scale);
      const float cardH = std::max(floorH, std::ceil(required));
      if (cardH <= maxCard + 0.5f) {
        out.summaryLines = sl;
        out.bodyLines = bl;
        out.summaryHeightPx = sumH;
        out.cardHeight = cardH;
        return out;
      }
    }

    Label summaryProbe;
    summaryProbe.setFontSize(summaryFontSize(scale));
    summaryProbe.setFontWeight(FontWeight::Bold);
    summaryProbe.setMaxWidth(textMaxWidth);
    summaryProbe.setText(displaySummary);
    summaryProbe.setMaxLines(1);
    summaryProbe.measure(rc);

    Label bodyProbe;
    bodyProbe.setFontSize(bodyFontSize(scale));
    bodyProbe.setMaxWidth(textMaxWidth);
    bodyProbe.setText(displayBody);
    bodyProbe.setMaxLines(1);
    bodyProbe.measure(rc);

    out.summaryLines = 1;
    out.bodyLines = 1;
    out.summaryHeightPx = summaryProbe.height();
    out.cardHeight = std::min(
        maxCard,
        std::max(
            floorH, std::ceil(requiredToastCardHeight(out.summaryHeightPx, bodyProbe.height(), actionsReserved, scale))
        )
    );
    return out;
  }

  void clampBodyLabelHeight(Label& bodyLabel, float maxBodyHeight) {
    if (maxBodyHeight <= 0.0f) {
      bodyLabel.setText("");
      bodyLabel.setVisible(false);
      return;
    }

    bodyLabel.setClipChildren(true);
    bodyLabel.setSize(bodyLabel.width(), std::max(1.0f, std::floor(maxBodyHeight)));
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

  std::int32_t horizontalLayerMarginFromScreenMargin(int offsetX, float scale) {
    return static_cast<std::int32_t>(offsetX) - horizontalRevealPadding(scale);
  }

  ToastSurfaceMargins toastSurfaceMargins(std::string_view position, int offsetX, int offsetY, float scale) {
    const auto sideMargin = horizontalLayerMarginFromScreenMargin(offsetX, scale);
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
    addPopup(n);
    break;
  case NotificationEvent::Updated: {
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
      if (m_entries[i].notificationId == n.id && !m_entries[i].exiting) {
        const bool actionSetChanged = (m_entries[i].actions != n.actions) || (m_entries[i].icon != n.icon);
        const bool imageDataChanged = (m_entries[i].imageData != n.imageData);
        const float previousHeight = m_entries[i].height;
        const int prevToastSummaryLines = m_entries[i].toastSummaryLines;
        const int prevToastBodyLines = m_entries[i].toastBodyLines;
        const bool previouslyPlaced = hasPlacement(m_entries[i]);
        m_entries[i].appName = n.appName;
        m_entries[i].summary = n.summary;
        m_entries[i].body = n.body;
        m_entries[i].actions = n.actions;
        m_entries[i].icon = n.icon;
        m_entries[i].imageData = n.imageData;
        refreshEntryGeometry(m_entries[i]);
        m_entries[i].rawTimeoutMs = n.timeout;
        const bool layoutChanged = actionSetChanged || imageDataChanged ||
                                   std::abs(previousHeight - m_entries[i].height) > 0.5f ||
                                   prevToastSummaryLines != m_entries[i].toastSummaryLines ||
                                   prevToastBodyLines != m_entries[i].toastBodyLines;
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
            const float preservedReveal = cardReveal(cs, m_entries[i].height);
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

            if (inst->sceneRoot != nullptr) {
              inst->sceneRoot->removeChild(cs.cardNode);
            }

            cs = {};
            InputArea* rebuilt = buildCard(
                m_entries[i], &cs.cardContent, &cs.cardForeground, &cs.appNameLabel, &cs.summaryLabel, &cs.bodyLabel,
                &cs.cardBg, &cs.appIconNode, &cs.progressBar, &cs.closeGlyph, &cs.actionsRowNode,
                &cs.inlineReplyRowNode, &cs.inlineReplyInput
            );
            cs.cardNode = rebuilt;
            const float revealY = hasPlacement(m_entries[i])
                                      ? entryYForSurface(m_entries[i], static_cast<float>(inst->surface->height()))
                                      : 0.0f;
            applyCardReveal(cs, preservedReveal, revealY, m_entries[i].height);
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
              const float targetY = entryYForSurface(m_entries[i], static_cast<float>(inst->surface->height()));
              Instance* instPtr = inst.get();
              cs.entryAnimId = inst->animations.animate(
                  preservedReveal, 1.0f, Style::animNormal, Easing::EaseOutCubic,
                  [this, viewport = cs.cardNode, content = cs.cardContent, foreground = cs.cardForeground, targetY,
                   cardHeight = m_entries[i].height, scale = notificationUiScale(m_config)](float v) {
                    applyCardRevealNodes(
                        viewport, content, foreground, v, targetY, revealDirection(), cardHeight, scale
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
          } else if (!layoutChanged) {
            cs.appNameLabel->setText(n.appName);
            const float scale = notificationUiScale(m_config);
            const float actionsReservedHeight = measureActionsFromPairs(*m_renderContext, m_entries[i].actions, scale);
            PopupEntry& e = m_entries[i];
            const std::string displaySummary = StringUtils::trimLeadingBlankLines(e.summary);
            const std::string displayBody = StringUtils::trimLeadingBlankLines(e.body);
            cs.summaryLabel->setText(displaySummary);
            cs.summaryLabel->setMaxLines(std::max(1, e.toastSummaryLines));
            cs.summaryLabel->measure(*m_renderContext);
            const float summaryH = cs.summaryLabel->height();
            const float bodyHeight = availableBodyHeight(summaryH, actionsReservedHeight, cs.cardNode->height(), scale);
            const int bodyLines = e.toastBodyLines;
            cs.bodyLabel->setMaxLines(std::max(1, bodyLines));
            cs.bodyLabel->setText(bodyLines > 0 ? displayBody : "");
            cs.bodyLabel->measure(*m_renderContext);
            cs.bodyLabel->setVisible(bodyLines > 0 && !StringUtils::isBlank(displayBody));
            cs.bodyLabel->setPosition(notificationTextStartX(scale), bodyTopForSummary(summaryH, scale));
            clampBodyLabelHeight(*cs.bodyLabel, bodyHeight);
          }

          // Reset countdown
          if (cs.countdownAnimId != 0) {
            inst->animations.cancel(cs.countdownAnimId);
          }
          const int newDuration = resolveDisplayDuration(n.timeout);
          m_entries[i].displayDurationMs = newDuration;
          m_entries[i].remainingProgress = 1.0f;
          if (newDuration < 0) {
            cs.progressBar->setOpacity(0.0f);
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
    removePopup(n.id);
    break;
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
  entry.appName = n.appName;
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
  const auto it = std::find_if(m_entries.begin(), m_entries.end(), [notificationId](const PopupEntry& entry) {
    return entry.notificationId == notificationId;
  });
  if (it == m_entries.end()) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(std::distance(m_entries.begin(), it));

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
    revealQueuedEntries();
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
      entry, &cs.cardContent, &cs.cardForeground, &cs.appNameLabel, &cs.summaryLabel, &cs.bodyLabel, &cs.cardBg,
      &cs.appIconNode, &cs.progressBar, &cs.closeGlyph, &cs.actionsRowNode, &cs.inlineReplyRowNode, &cs.inlineReplyInput
  );
  cs.cardNode = card;

  const float targetY = entryYForSurface(entry, static_cast<float>(inst.surface->height()));
  applyCardReveal(cs, 0.0f, targetY, entry.height);

  inst.sceneRoot->addChild(std::unique_ptr<Node>(card));

  // Entry animation
  cs.entryAnimId = inst.animations.animate(
      0.0f, 1.0f, Style::animNormal, Easing::EaseOutCubic,
      [this, viewport = cs.cardNode, content = cs.cardContent, foreground = cs.cardForeground, targetY,
       cardHeight = entry.height, scale = notificationUiScale(m_config)](float v) {
        applyCardRevealNodes(viewport, content, foreground, v, targetY, revealDirection(), cardHeight, scale);
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
    cs.progressBar->setOpacity(0.0f);
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

  // Hover wiring: pause countdown while the card is hovered, and brighten the (X).
  // On leave, resume the countdown from the remaining progress.
  const bool isCritical = (entry.urgency == Urgency::Critical);
  const Color closeColorNormal = resolveColorSpec(
      isCritical ? colorSpecFromRole(ColorRole::Error, 0.75f) : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.6f)
  );
  const Color closeColorHover =
      resolveColorSpec(isCritical ? colorSpecFromRole(ColorRole::Error) : colorSpecFromRole(ColorRole::OnSurface));
  const int totalDuration = entry.displayDurationMs;
  const uint32_t notificationId = entry.notificationId;
  Glyph* closeGlyphPtr = cs.closeGlyph;
  ProgressBar* progressBarPtr = cs.progressBar;
  InputArea* cardInput = card;
  const bool hasDefaultAction = !entry.actions.empty() && entry.actions.size() >= 2 && entry.actions[0] == "default";

  card->setOnEnter([this, closeGlyphPtr, closeColorNormal, closeColorHover, notificationId, progressBarPtr, cardInput,
                    hasDefaultAction, scale = notificationUiScale(m_config)](const InputArea::PointerData& data) {
    const bool closeHovered = isCloseButtonHit(data.localX, data.localY, scale);
    closeGlyphPtr->setColor(closeHovered ? closeColorHover : closeColorNormal);
    cardInput->setCursorShape(
        (closeHovered || hasDefaultAction) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                                           : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
    );
    if (auto* popup = findEntry(notificationId); popup != nullptr) {
      popup->hovered = true;
      popup->remainingProgress = std::clamp(progressBarPtr->progress(), 0.0f, 1.0f);
    }
    pauseCountdowns(notificationId);
    // Pause the server-side expiry — otherwise NotificationManager's own timer
    // would fire Closed behind our back, which is what "the progress bar stops
    // but the timer keeps running" was.
    if (m_notifications != nullptr) {
      m_notifications->pauseExpiry(notificationId);
    }
  });

  card->setOnMotion([closeGlyphPtr, closeColorNormal, closeColorHover, cardInput, hasDefaultAction,
                     scale = notificationUiScale(m_config)](const InputArea::PointerData& data) {
    const bool closeHovered = isCloseButtonHit(data.localX, data.localY, scale);
    closeGlyphPtr->setColor(closeHovered ? closeColorHover : closeColorNormal);
    cardInput->setCursorShape(
        (closeHovered || hasDefaultAction) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                                           : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
    );
  });

  card->setOnLeave([this, notificationId, totalDuration, closeGlyphPtr, closeColorNormal, progressBarPtr, cardInput]() {
    closeGlyphPtr->setColor(closeColorNormal);
    cardInput->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    if (auto* popup = findEntry(notificationId); popup != nullptr) {
      popup->hovered = false;
      popup->remainingProgress = std::clamp(progressBarPtr->progress(), 0.0f, 1.0f);
    }
    if (totalDuration < 0) {
      return;
    }
    if (auto* popupAfterLeave = findEntry(notificationId);
        popupAfterLeave != nullptr && popupAfterLeave->replyInputFocused) {
      return;
    }
    const float remaining = std::clamp(progressBarPtr->progress(), 0.0f, 1.0f);
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

void NotificationToast::syncEntryVisibility(std::size_t entryIndex) {
  if (entryIndex >= m_entries.size()) {
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
    const bool shouldShow = hasPlacement(m_entries[entryIndex]) &&
                            fitsOnSurface(m_entries[entryIndex], static_cast<float>(inst->surface->height()));
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
  const float cardHeight = (entryIndex < m_entries.size()) ? m_entries[entryIndex].height : card->height();
  const float startReveal = cardReveal(cs, cardHeight);
  const float targetY = card->y();
  const uint32_t removingId = (entryIndex < m_entries.size()) ? m_entries[entryIndex].notificationId : 0;

  cs.exitAnimId = inst.animations.animate(
      startReveal, 0.0f, Style::animNormal, Easing::EaseInOutQuad,
      [this, card, content, foreground, targetY, cardHeight, scale = notificationUiScale(m_config)](float v) {
        applyCardRevealNodes(card, content, foreground, v, targetY, revealDirection(), cardHeight, scale);
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
  const auto it = std::find_if(m_entries.begin(), m_entries.end(), [notificationId](const PopupEntry& entry) {
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
    const bool isDriver = (m_instances.size() > 0 && m_instances[0].get() == inst.get());
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

float NotificationToast::entryHeight(const PopupEntry& entry) const {
  if (entry.height > 0.5f) {
    return entry.height;
  }
  return cardHeightForEntry(!entry.actions.empty(), notificationUiScale(m_config));
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
  return std::any_of(selectedMonitors.begin(), selectedMonitors.end(), [&output](const std::string& match) {
    return outputMatchesSelector(match, output);
  });
}

bool NotificationToast::isBottomStacking() const { return isBottomPosition(notificationPosition()); }

NotificationToast::RevealDirection NotificationToast::revealDirection() const {
  return revealDirectionForPosition(notificationPosition());
}

void NotificationToast::refreshEntryGeometry(PopupEntry& entry) const {
  if (m_renderContext == nullptr) {
    entry.toastSummaryLines = kMaxSummaryLines;
    entry.toastBodyLines = 0;
    entry.height = cardHeightForEntry(!entry.actions.empty(), notificationUiScale(m_config));
    return;
  }

  const float scale = notificationUiScale(m_config);
  const ToastGeometry planned = planToastLayout(
      *m_renderContext, entry.summary, entry.body, entry.actions, cardHeightForEntry(!entry.actions.empty(), scale),
      scale
  );
  entry.toastSummaryLines = planned.summaryLines;
  entry.toastBodyLines = planned.bodyLines;
  entry.height = planned.cardHeight;
}

float NotificationToast::layoutBottomForSurfaceHeight(float surfaceHeight) const {
  const float scale = notificationUiScale(m_config);
  const float edgePadding = isBottomStacking() ? 0.0f : paddingBottom(scale);
  return std::max(paddingTop(scale), surfaceHeight - edgePadding);
}

float NotificationToast::entryOffsetFromPlacementBottom(const PopupEntry& entry) const {
  return maxPlacementBottom() - entry.y;
}

float NotificationToast::entryYForSurface(const PopupEntry& entry, float surfaceHeight) const {
  if (!hasPlacement(entry)) {
    return entry.y;
  }
  if (isBottomStacking()) {
    return layoutBottomForSurfaceHeight(surfaceHeight) - entryOffsetFromPlacementBottom(entry);
  }
  return entry.y;
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
      if (output.output == nullptr) {
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
    std::sort(occupied.begin(), occupied.end(), [](const Interval& a, const Interval& b) {
      return a.bottom > b.bottom;
    });
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

  std::sort(occupied.begin(), occupied.end(), [](const Interval& a, const Interval& b) { return a.top < b.top; });
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
  const auto surfaceWidth = ::surfaceWidth(scale);
  const std::string position = notificationPosition();
  const std::string layer = notificationLayer();
  const auto selectedMonitors = notificationMonitors();
  const auto& notifCfg = m_config->config().notification;
  const int offX = std::max(0, notifCfg.offsetX);
  const int offY = std::max(0, notifCfg.offsetY);
  const std::uint32_t anchor = toastSurfaceAnchor(position);
  const ToastSurfaceMargins margins = toastSurfaceMargins(position, offX, offY, scale);
  if (!m_instances.empty() &&
      (position != m_lastPosition || layer != m_lastLayer || selectedMonitors != m_lastMonitorSelectors)) {
    for (auto& inst : m_instances) {
      inst->animations.cancelAll();
      inst->inputDispatcher.setSceneRoot(nullptr);
    }
    m_instances.clear();
  }
  m_lastPosition = position;
  m_lastLayer = layer;
  m_lastMonitorSelectors = selectedMonitors;

  for (const auto& output : m_wayland->outputs()) {
    if (output.output == nullptr) {
      continue;
    }
    if (!shouldRenderOnOutput(output)) {
      continue;
    }

    auto existingIt = std::find_if(m_instances.begin(), m_instances.end(), [&output](const auto& inst) {
      return inst != nullptr && inst->output == output.output;
    });
    if (existingIt != m_instances.end()) {
      auto& inst = *existingIt;
      inst->scale = output.scale;
      if (inst->surface != nullptr) {
        if (inst->surface->marginTop() != margins.top || inst->surface->marginRight() != margins.right ||
            inst->surface->marginBottom() != margins.bottom || inst->surface->marginLeft() != margins.left) {
          inst->surface->setMargins(margins.top, margins.right, margins.bottom, margins.left);
        }
        if (inst->surface->width() != surfaceWidth) {
          inst->surface->requestSize(surfaceWidth, 0);
        }
      }
      continue;
    }

    auto inst = std::make_unique<Instance>();
    inst->output = output.output;
    inst->scale = output.scale;

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

  const bool needsSceneBuild = inst.sceneRoot == nullptr ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->width())) != width ||
                               static_cast<uint32_t>(std::round(inst.sceneRoot->height())) != height;
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

  auto sceneRoot = std::make_unique<Node>();
  sceneRoot->setSize(w, h);
  sceneRoot->setAnimationManager(&inst.animations);

  inst.inputDispatcher.setSceneRoot(sceneRoot.get());
  inst.sceneRoot = std::move(sceneRoot);
  inst.inputDispatcher.setCursorShapeCallback([this](uint32_t serial, uint32_t shape) {
    m_wayland->setCursorShape(serial, shape);
  });

  inst.surface->setSceneRoot(inst.sceneRoot.get());

  // Build cards for any entries that already exist
  inst.cards.clear();
  inst.cards.resize(m_entries.size());
  for (std::size_t i = 0; i < m_entries.size(); ++i) {
    if (!m_entries[i].exiting && hasPlacement(m_entries[i]) &&
        fitsOnSurface(m_entries[i], static_cast<float>(height))) {
      addCardToInstance(inst, i);
    }
  }

  updateInputRegion(inst);
  if (inst.pointerInside) {
    inst.inputDispatcher.pointerMotion(inst.lastPointerX, inst.lastPointerY, 0);
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
  inst.surface->setBlurRegion(blurRects);
}

float NotificationToast::cardReveal(const Instance::CardState& cs, float cardHeight) const {
  return cardRevealFromNode(cs.cardNode, revealDirection(), cardHeight, notificationUiScale(m_config));
}

void NotificationToast::applyCardReveal(Instance::CardState& cs, float reveal, float y, float cardHeight) const {
  applyCardRevealNodes(
      cs.cardNode, cs.cardContent, cs.cardForeground, reveal, y, revealDirection(), cardHeight,
      notificationUiScale(m_config)
  );
}

InputArea* NotificationToast::buildCard(
    const PopupEntry& entry, Node** outCardContent, Node** outCardForeground, Label** outAppName, Label** outSummary,
    Label** outBody, Node** outBg, Node** outAppIcon, ProgressBar** outProgress, Glyph** outCloseGlyph,
    Node** outActionsRow, Node** outInlineReplyRow, Input** outInlineReplyInput
) {
  const float scale = notificationUiScale(m_config);
  const bool hasInlineReply = hasInlineReplyAction(entry.actions);
  const float cardHeight = entry.height > 0.5f ? entry.height : cardHeightForEntry(!entry.actions.empty(), scale);
  const float cardW = cardWidth(scale);
  const float innerWidth = cardW - cardInnerPad(scale) * 2.0f;
  const float progressY = cardHeight - progressHeight(scale) - progressBottomMargin(scale);

  auto viewport = std::make_unique<InputArea>();
  viewport->setSize(cardW, cardHeight);
  viewport->setClipChildren(true);
  viewport->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
  // Right-clicking anywhere dismisses the card, while the visual (X) keeps its
  // familiar left-click close affordance without adding a nested hover target.
  viewport->setOnClick([this, id = entry.notificationId,
                        hasDefaultAction =
                            !entry.actions.empty() && entry.actions.size() >= 2 && entry.actions[0] == "default",
                        scale](const InputArea::PointerData& data) {
    if (data.button == BTN_RIGHT || (data.button == BTN_LEFT && isCloseButtonHit(data.localX, data.localY, scale))) {
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

  auto cardRoot = std::make_unique<Node>();
  cardRoot->setSize(cardW, cardHeight);
  *outCardContent = cardRoot.get();

  auto foreground = std::make_unique<Node>();
  foreground->setSize(cardW, cardHeight);
  *outCardForeground = foreground.get();

  const bool isCritical = (entry.urgency == Urgency::Critical);
  const float textStartX = notificationTextStartX(scale);
  const float textMaxWidth = notificationTextMaxWidth(scale);
  const float bgAlpha = m_config != nullptr ? m_config->config().notification.backgroundOpacity : 0.97f;

  // Background
  *outBg = cardRoot->addChild(
      ui::box({
          .width = cardW,
          .height = cardHeight,
          .configure = [isCritical, scale, bgAlpha](Box& box) {
            box.setCardStyle();
            box.setRadius(Style::scaledRadiusXl(scale));
            box.setFill(colorSpecFromRole(ColorRole::Surface, bgAlpha));
            if (isCritical) {
              // Keep critical toasts readable: surface background + urgent border.
              box.setBorder(colorSpecFromRole(ColorRole::Error, 0.95f), Style::borderWidth * 1.4f);
            } else {
              box.setBorder(colorSpecFromRole(ColorRole::Outline, 0.8f), Style::borderWidth);
            }
          },
      })
  );

  // Header row: app name (left) + close glyph (right), vertically centred via Flex
  auto headerRow = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .width = innerWidth,
      .height = closeButtonSize(scale),
      .configure = [scale](Flex& row) { row.setPosition(cardInnerPad(scale), cardInnerPad(scale)); },
  });

  auto headerLeft = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceXs * scale,
  });

  auto iconSlot = std::make_unique<Node>();
  iconSlot->setSize(notificationIconSize(scale), notificationIconSize(scale));
  iconSlot->setPosition(cardInnerPad(scale), std::round((cardHeight - notificationIconSize(scale)) * 0.5f));

  bool iconAssigned = false;
  if (entry.icon.has_value()) {
    const std::string& rawIcon = *entry.icon;
    if (rawIcon.size() > kNoctaliaGlyphIconPrefix.size() &&
        std::string_view(rawIcon.data(), kNoctaliaGlyphIconPrefix.size()) == kNoctaliaGlyphIconPrefix) {
      const std::string_view glyphName(
          rawIcon.data() + kNoctaliaGlyphIconPrefix.size(), rawIcon.size() - kNoctaliaGlyphIconPrefix.size()
      );
      if (!glyphName.empty()) {
        *outAppIcon = iconSlot->addChild(
            ui::glyph({
                .glyph = std::string(glyphName),
                .glyphSize = kNotificationIconGlyphSize * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .configure = [this, scale](Glyph& glyph) {
                  glyph.measure(*m_renderContext);
                  glyph.setPosition(
                      std::round((notificationIconSize(scale) - glyph.width()) * 0.5f),
                      std::round((notificationIconSize(scale) - glyph.height()) * 0.5f)
                  );
                },
            })
        );
        iconAssigned = true;
      }
    }
  }
  if (!iconAssigned) {
    const std::string iconPath = resolveNotificationIconPath(entry);
    if (!iconPath.empty()) {
      auto appIcon = ui::image({
          .fit = ImageFit::Cover,
          .radius = notificationIconRadius(notificationIconSize(scale), scale),
          .width = notificationIconSize(scale),
          .height = notificationIconSize(scale),
          .configure = [](Image& image) { image.setPosition(0.0f, 0.0f); },
      });
      if (appIcon->setSourceFile(
              *m_renderContext, iconPath, static_cast<int>(std::round(notificationIconSize(scale)))
          )) {
        *outAppIcon = iconSlot->addChild(std::move(appIcon));
        iconAssigned = true;
      } else {
        kLog.warn("notification toast: failed to load icon image for #{} from '{}'", entry.notificationId, iconPath);
      }
    } else if (entry.imageData.has_value()) {
      const auto& image = *entry.imageData;
      if (image.width > 0 && image.height > 0 && !image.data.empty()) {
        auto appIcon = ui::image({
            .fit = ImageFit::Cover,
            .radius = notificationIconRadius(notificationIconSize(scale), scale),
            .width = notificationIconSize(scale),
            .height = notificationIconSize(scale),
            .configure = [](Image& control) { control.setPosition(0.0f, 0.0f); },
        });
        const bool validImageMetadata = image.bitsPerSample == 8 && ((image.channels == 4 && image.hasAlpha) ||
                                                                     (image.channels == 3 && !image.hasAlpha));
        const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
        if (validImageMetadata && appIcon->setSourceRaw(
                                      *m_renderContext, image.data.data(), image.data.size(), image.width, image.height,
                                      image.rowStride, format, true
                                  )) {
          *outAppIcon = iconSlot->addChild(std::move(appIcon));
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
  }

  if (!iconAssigned) {
    *outAppIcon = iconSlot->addChild(
        ui::glyph({
            .glyph = "bell",
            .glyphSize = kNotificationIconGlyphSize * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .configure = [this, scale](Glyph& glyph) {
              glyph.measure(*m_renderContext);
              glyph.setPosition(
                  std::round((notificationIconSize(scale) - glyph.width()) * 0.5f),
                  std::round((notificationIconSize(scale) - glyph.height()) * 0.5f)
              );
            },
        })
    );
  }

  foreground->addChild(std::move(iconSlot));

  auto appName = ui::label({
      .text = entry.appName,
      .fontSize = metaFontSize(scale),
      .color = colorSpecFromRole(isCritical ? ColorRole::Error : ColorRole::OnSurfaceVariant),
      .maxWidth = innerWidth - closeButtonSize(scale) - Style::spaceXs * scale,
  });
  appName->measure(*m_renderContext);
  *outAppName = appName.get();
  headerLeft->addChild(std::move(appName));
  headerLeft->layout(*m_renderContext);
  headerRow->addChild(std::move(headerLeft));

  *outCloseGlyph = static_cast<Glyph*>(headerRow->addChild(
      ui::glyph({
          .glyph = "close",
          .glyphSize = kCloseGlyphSize * scale,
          .color = fixedColorSpec(resolveColorSpec(
              isCritical ? colorSpecFromRole(ColorRole::Error, 0.75f)
                         : colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.6f)
          )),
      })
  ));
  headerRow->layout(*m_renderContext);

  foreground->addChild(std::move(headerRow));

  // Summary (bold title) — Pango handles wrap + ellipsize.
  const std::string displaySummary = StringUtils::trimLeadingBlankLines(entry.summary);
  const std::string displayBody = StringUtils::trimLeadingBlankLines(entry.body);
  auto summary = ui::label({
      .text = displaySummary,
      .fontSize = summaryFontSize(scale),
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxWidth = textMaxWidth,
      .fontWeight = FontWeight::Bold,
  });
  std::unique_ptr<Flex> actionsRow;
  std::unique_ptr<Flex> inlineReplyRow;
  std::unique_ptr<Input> inlineReplyInput;
  std::unique_ptr<Button> inlineReplySendButton;
  Input* inlineReplyInputPtr = nullptr;
  float actionsReservedHeight = 0.0f;
  if (!entry.actions.empty()) {
    const uint32_t notificationId = entry.notificationId;
    const int totalDuration = entry.displayDurationMs;

    // Build action buttons row (always visible initially)
    {
      std::vector<std::unique_ptr<Button>> buttons;
      for (std::size_t i = 0; i + 1 < entry.actions.size() && static_cast<int>(buttons.size()) < kMaxActionButtons;
           i += 2) {
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
        actionButton->setOnEnter([this, notificationId]() {
          pauseCountdowns(notificationId);
          if (m_notifications != nullptr) {
            m_notifications->pauseExpiry(notificationId);
          }
        });
        actionButton->setOnLeave([this, notificationId, totalDuration]() {
          if (totalDuration < 0) {
            return;
          }
          const auto* popup = findEntry(notificationId);
          if (popup == nullptr || popup->replyInputFocused) {
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
        actionsRow = ui::makeFlex(FlexDirection::Horizontal, {});
        actionsReservedHeight = layoutNotificationActionsRow(*m_renderContext, *actionsRow, buttons, scale);
      }
    }

    // Inline reply row (hidden until the user taps Reply).
    if (hasInlineReply) {
      const uint32_t replyNotificationId = entry.notificationId;
      const int replyTotalDuration = entry.displayDurationMs;
      inlineReplyRow = ui::row({
          .align = FlexAlign::Center,
          .gap = kInlineReplyGap * scale,
          .visible = false,
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
                  if (replyTotalDuration < 0) {
                    return;
                  }
                  if (auto* popup = findEntry(replyNotificationId); popup != nullptr && popup->hovered) {
                    return;
                  }
                  const auto* popup = findEntry(replyNotificationId);
                  if (popup == nullptr) {
                    return;
                  }
                  const float remaining = std::clamp(popup->remainingProgress, 0.0f, 1.0f);
                  if (remaining <= 0.0f) {
                    if (m_notifications != nullptr) {
                      m_notifications->resumeExpiry(replyNotificationId, 0);
                    }
                    return;
                  }
                  const int32_t remainingMs = std::max<int32_t>(
                      1, static_cast<int32_t>(std::ceil(static_cast<float>(replyTotalDuration) * remaining))
                  );
                  if (m_notifications != nullptr) {
                    m_notifications->resumeExpiry(replyNotificationId, remainingMs);
                  }
                  resumeCountdowns(replyNotificationId);
                });
                // Pointer hover must not restyle the field; only real focus should show the active chrome.
                replyInputArea->setOnEnter({});
                replyInputArea->setOnLeave({});
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
          .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER); },
      });

      inlineReplyRow->setSize(textMaxWidth, 0.0f);
      inlineReplyRow->addChild(std::move(inlineReplyInput));
      inlineReplyRow->addChild(std::move(inlineReplySendButton));
      inlineReplyRow->layout(*m_renderContext);
      inlineReplyInput = nullptr;
    }
  }

  summary->setMaxLines(std::max(1, entry.toastSummaryLines));
  summary->measure(*m_renderContext);
  summary->setPosition(textStartX, cardInnerPad(scale) + closeButtonSize(scale) + metaGap(scale));
  const float summaryMeasuredH = summary->height();
  *outSummary = summary.get();
  foreground->addChild(std::move(summary));

  auto body = ui::label({
      .text = displayBody,
      .fontSize = bodyFontSize(scale),
      .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      .maxWidth = textMaxWidth,
  });
  const float bodyHeight = availableBodyHeight(summaryMeasuredH, actionsReservedHeight, cardHeight, scale);
  const int bodyLines = entry.toastBodyLines;
  body->setMaxLines(std::max(1, bodyLines));
  if (bodyLines <= 0) {
    body->setText("");
    body->setVisible(false);
  }
  body->measure(*m_renderContext);
  body->setPosition(textStartX, bodyTopForSummary(summaryMeasuredH, scale));
  clampBodyLabelHeight(*body, bodyHeight);
  *outBody = body.get();
  foreground->addChild(std::move(body));

  if (actionsRow != nullptr) {
    actionsRow->setPosition(textStartX, progressY - actionsRow->height() - actionRowGap(scale));
    *outActionsRow = actionsRow.get();
    foreground->addChild(std::move(actionsRow));
  } else {
    *outActionsRow = nullptr;
  }

  if (inlineReplyRow != nullptr) {
    inlineReplyRow->setPosition(textStartX, progressY - inlineReplyRow->height() - actionRowGap(scale));
    *outInlineReplyRow = inlineReplyRow.get();
    *outInlineReplyInput = inlineReplyInputPtr;
    foreground->addChild(std::move(inlineReplyRow));
  } else {
    *outInlineReplyRow = nullptr;
    *outInlineReplyInput = nullptr;
  }

  // Progress bar (countdown)
  *outProgress = static_cast<ProgressBar*>(foreground->addChild(
      ui::progressBar({
          .fill = colorSpecFromRole(isCritical ? ColorRole::Error : ColorRole::Primary),
          .track = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.35f),
          .width = innerWidth,
          .height = progressHeight(scale),
          .configure = [scale, progressY](ProgressBar& progressBar) {
            progressBar.setPosition(cardInnerPad(scale), progressY);
          },
      })
  ));

  cardRoot->addChild(std::move(foreground));
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
      }
      if (cs.inlineReplyRowNode != nullptr) {
        cs.inlineReplyRowNode->setVisible(true);
      }
      if (inst->surface != nullptr) {
        inst->surface->requestRedraw();
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
  if (target->sceneRoot != nullptr && (target->sceneRoot->paintDirty() || target->sceneRoot->layoutDirty()) &&
      target->surface != nullptr) {
    target->surface->requestRedraw();
  }
  return true;
}

// --- Pointer events ---

bool NotificationToast::onPointerEvent(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Button && event.state == 1) {
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
        const bool pressed = (event.state == 1);
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
  if (entry.icon->size() > kNoctaliaGlyphIconPrefix.size() &&
      std::string_view(entry.icon->data(), kNoctaliaGlyphIconPrefix.size()) == kNoctaliaGlyphIconPrefix) {
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

    if (m_failedRemoteIconDownloads.find(iconValue) != m_failedRemoteIconDownloads.end()) {
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

    if (m_httpClient != nullptr && m_pendingRemoteIconDownloads.find(iconValue) == m_pendingRemoteIconDownloads.end()) {
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
      localPath, static_cast<int>(std::round(notificationIconSize(notificationUiScale(m_config))))
  );
  if (!resolved.empty()) {
    return resolved;
  }

  kLog.warn("notification toast: #{} theme icon not found name='{}'", entry.notificationId, localPath);
  return {};
}
