#include "shell/bar/widgets/active_window_widget.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/app_identity.h"
#include "system/desktop_entry.h"
#include "system/internal_app_metadata.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <string_view>

ActiveWindowWidget::ActiveWindowWidget(
    ConfigService& config, CompositorPlatform& platform, float maxWidth, float minWidth, float iconSize,
    ActiveWindowTitleScrollMode titleScrollMode, ActiveWindowDisplayMode displayMode, bool showEmptyLabel
)
    : m_config(config), m_platform(platform), m_maxWidth(maxWidth), m_minWidth(minWidth), m_iconSize(iconSize),
      m_titleScrollMode(titleScrollMode), m_displayMode(displayMode), m_showEmptyLabel(showEmptyLabel) {
  buildDesktopIconIndex();
}

void ActiveWindowWidget::create() {
  auto rootNode = std::make_unique<InputArea>();
  rootNode->setOnEnter([this](const InputArea::PointerData&) {
    applyTitleScrollMode(m_title != nullptr && m_title->visible());
    requestUpdate();
  });
  rootNode->setOnLeave([this]() {
    applyTitleScrollMode(m_title != nullptr && m_title->visible());
    requestUpdate();
  });
  m_area = rootNode.get();

  rootNode->addChild(
      ui::image({
          .out = &m_icon,
          .fit = ImageFit::Contain,
          .radius = Style::radiusSm,
          .width = m_iconSize * m_contentScale,
          .height = m_iconSize * m_contentScale,
      })
  );

  rootNode->addChild(
      ui::label({
          .out = &m_title,
          .fontSize = Style::fontSizeBody * m_contentScale,
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
          .fontWeight = labelFontWeight(),
          .autoScroll = false,
      })
  );

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() {
    m_iconColorizeRefreshPending = true;
    requestUpdate();
  });

  setRoot(std::move(rootNode));
}

void ActiveWindowWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr || m_icon == nullptr || m_title == nullptr) {
    return;
  }
  syncState(renderer);

  if (!rootNode->visible() || !rootNode->participatesInLayout()) {
    return;
  }

  const bool showingEmptyPlaceholder = m_lastEmptyState && m_showEmptyLabel;
  const bool isVertical = containerHeight > containerWidth;
  const float iconSize = m_iconSize * m_contentScale;
  const float maxLength = std::max(0.0f, m_maxWidth * m_contentScale);
  const float minLength = std::clamp(m_minWidth * m_contentScale, 0.0f, maxLength);
  m_icon->setSize(iconSize, iconSize);

  m_title->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));

  if (isVertical && !showingEmptyPlaceholder) {
    m_title->setVisible(false);
    applyTitleScrollMode(false);
    m_icon->setVisible(true);
    m_icon->setPosition(0.0f, 0.0f);
    rootNode->setSize(m_icon->width(), m_icon->height());
  } else {
    const bool showIcon = !showingEmptyPlaceholder && m_displayMode != ActiveWindowDisplayMode::TextOnly;
    const bool showTitle =
        showingEmptyPlaceholder || (m_displayMode != ActiveWindowDisplayMode::IconOnly && !m_lastTitle.empty());
    m_icon->setVisible(showIcon);
    m_title->setVisible(showTitle);
    applyTitleScrollMode(showTitle);
    const float spacing = showIcon && showTitle ? Style::spaceXs : 0.0f;
    const float iconWidth = showIcon ? m_icon->width() : 0.0f;
    const float labelMaxWidth = showTitle ? std::max(0.0f, maxLength - iconWidth - spacing) : 0.0f;
    m_title->setMaxWidth(labelMaxWidth);
    m_title->measure(renderer);

    const float iconHeight = showIcon ? m_icon->height() : 0.0f;
    const float titleHeight = showTitle ? m_title->height() : 0.0f;
    const float contentHeight = std::max(iconHeight, titleHeight);
    const float iconY = showIcon ? std::round((contentHeight - m_icon->height()) * 0.5f) : 0.0f;
    const float labelY = std::round((contentHeight - m_title->height()) * 0.5f);

    m_icon->setPosition(0.0f, iconY);
    m_title->setPosition(showIcon ? m_icon->width() + spacing : 0.0f, labelY);

    const float contentWidth = showTitle ? m_title->x() + m_title->width() : iconWidth;
    rootNode->setSize(std::clamp(contentWidth, minLength, maxLength), contentHeight);
  }
}

void ActiveWindowWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void ActiveWindowWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
      rootNode->setVisible(showWidget);
      rootNode->setParticipatesInLayout(showWidget);
      if (!showWidget) {
        rootNode->setSize(0.0f, 0.0f);
      }
      requestUpdate();
    } else if (!showWidget && (rootNode->width() > 0.0f || rootNode->height() > 0.0f)) {
      rootNode->setSize(0.0f, 0.0f);
    }
  }
}

void ActiveWindowWidget::applyTitleScrollMode(bool titleVisible) {
  if (m_title == nullptr) {
    return;
  }

  const bool shouldScroll = titleVisible
      && (m_titleScrollMode == ActiveWindowTitleScrollMode::Always
          || (m_titleScrollMode == ActiveWindowTitleScrollMode::OnHover && m_area != nullptr && m_area->hovered()));
  m_title->setAutoScroll(shouldScroll);
  m_title->setAutoScrollOnlyWhenHovered(false);
}

void ActiveWindowWidget::syncState(Renderer& renderer) {
  if (m_icon == nullptr || m_title == nullptr) {
    return;
  }

  const auto desktopVersion = desktopEntriesVersion();
  const bool desktopEntriesChanged = desktopVersion != m_desktopEntriesVersion;
  if (desktopEntriesChanged) {
    buildDesktopIconIndex();
  }

  const auto current = m_platform.activeToplevel();

  std::string identifier;
  std::string title;
  std::string appId;
  bool emptyState = false;

  if (!current.has_value()) {
    identifier = {};
    title = m_showEmptyLabel ? i18n::tr("bar.widgets.active-window.no-active-window") : std::string{};
    appId = {};
    emptyState = true;
  } else {
    identifier = current->identifier;
    title = StringUtils::windowTitleSingleLine(current->title);
    appId = current->appId;
    if (title.empty()) {
      title = appId;
    }
  }

  const bool showWidget = !emptyState || m_showEmptyLabel;
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    if (!m_iconColorizeRefreshPending && !desktopEntriesChanged && emptyState == m_lastEmptyState) {
      return;
    }
    m_iconColorizeRefreshPending = false;
    m_lastIdentifier = std::move(identifier);
    m_lastTitle = {};
    m_lastAppId = {};
    m_lastEmptyState = emptyState;
    m_lastIconPath = {};
    if (m_icon != nullptr) {
      m_icon->clear(renderer);
    }
    return;
  }

  if (!m_iconColorizeRefreshPending
      && !desktopEntriesChanged
      && identifier == m_lastIdentifier
      && title == m_lastTitle
      && appId == m_lastAppId
      && emptyState == m_lastEmptyState) {
    return;
  }
  m_iconColorizeRefreshPending = false;

  m_lastIdentifier = std::move(identifier);
  m_lastTitle = title;
  m_lastAppId = appId;
  m_lastEmptyState = emptyState;

  std::string iconPath = emptyState ? std::string{} : resolveIconPath(appId);

  m_title->setText(m_lastTitle);
  m_title->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_title->setVisible(!m_lastTitle.empty());
  applyTitleScrollMode(m_title->visible());
  m_title->measure(renderer);

  m_icon->setAppIconColorization(effectiveShellAppIconColorizationTint(m_config.config().shell));
  if (iconPath != m_lastIconPath) {
    m_lastIconPath = iconPath;
    if (!m_lastIconPath.empty()) {
      m_icon->setSourceFile(renderer, m_lastIconPath, static_cast<int>(std::round(48.0f * m_contentScale)), true);
    } else {
      m_icon->clear(renderer);
    }
  }

  requestUpdate();
}

std::string ActiveWindowWidget::resolveIconPath(const std::string& appId) {
  if (appId.empty()) {
    return {};
  }

  if (const auto internal = internal_apps::metadataForAppId(appId); internal.has_value()) {
    return internal->iconPath;
  }

  const app_identity::DesktopEntryLookupOptions lookupOptions = appId.starts_with("steam_app_")
      ? app_identity::DesktopEntryLookupOptions{.includeHidden = true, .includeNoDisplay = true}
      : app_identity::DesktopEntryLookupOptions{};
  if (const auto entry = app_identity::findDesktopEntry(appId, desktopEntries(), lookupOptions);
      entry.has_value() && !entry->icon.empty()) {
    const int iconTargetSize = static_cast<int>(std::round(48.0f * m_contentScale));
    const std::string& resolved = m_iconResolver.resolve(entry->icon, iconTargetSize);
    if (!resolved.empty()) {
      return resolved;
    }
  }

  const int iconTargetSize = static_cast<int>(std::round(48.0f * m_contentScale));
  auto resolveByName = [this, iconTargetSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }
    return m_iconResolver.resolve(name, iconTargetSize);
  };

  if (auto it = m_appIcons.find(appId); it != m_appIcons.end()) {
    const auto path = resolveByName(it->second);
    if (!path.empty()) {
      return path;
    }
  }

  const std::string appIdLower = StringUtils::toLower(appId);
  if (auto it = m_appIcons.find(appIdLower); it != m_appIcons.end()) {
    const auto path = resolveByName(it->second);
    if (!path.empty()) {
      return path;
    }
  }

  if (const auto slash = appId.find_last_of('/'); slash != std::string::npos && slash + 1 < appId.size()) {
    const std::string tail = appId.substr(slash + 1);
    if (auto it = m_appIcons.find(tail); it != m_appIcons.end()) {
      const auto path = resolveByName(it->second);
      if (!path.empty()) {
        return path;
      }
    }
  }

  return resolveByName(appId);
}

void ActiveWindowWidget::buildDesktopIconIndex() {
  m_appIcons.clear();
  auto addIndexKey = [this](std::string_view key, const std::string& icon) {
    if (key.empty() || icon.empty()) {
      return;
    }
    m_appIcons.try_emplace(std::string{key}, icon);
    m_appIcons.try_emplace(StringUtils::toLower(key), icon);
  };

  const auto& entries = desktopEntries();
  for (const auto& entry : entries) {
    if (entry.id.empty() || entry.icon.empty()) {
      continue;
    }

    addIndexKey(entry.id, entry.icon);
    if (const auto dot = entry.id.rfind('.'); dot != std::string::npos && dot + 1 < entry.id.size()) {
      addIndexKey(entry.id.substr(dot + 1), entry.icon);
    }
    // Common packaging suffixes in desktop IDs (e.g. vesktop-bin.desktop).
    if (const auto dash = entry.id.rfind('-'); dash != std::string::npos && dash + 1 < entry.id.size()) {
      const std::string suffix = entry.id.substr(dash + 1);
      if (suffix == "bin" || suffix == "desktop") {
        addIndexKey(entry.id.substr(0, dash), entry.icon);
      }
    }
    if (!entry.startupWmClass.empty()) {
      addIndexKey(entry.startupWmClass, entry.icon);
    }
  }
  m_desktopEntriesVersion = desktopEntriesVersion();
}
