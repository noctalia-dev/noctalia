#include "shell/tray/tray_drawer_panel.h"

#include "config/config_service.h"
#include "dbus/tray/tray_service.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/bar/widgets/tray_widget_definition.h"
#include "shell/panel/panel_manager.h"
#include "shell/tray/tray_identifier.h"
#include "util/string_utils.h"

#include <algorithm>
#include <vector>

TrayDrawerPanel::TrayDrawerPanel(TrayService* tray, ConfigService* config) : m_tray(tray), m_config(config) {}

TrayDrawerPanel::~TrayDrawerPanel() = default;

PanelPlacement TrayDrawerPanel::panelPlacement() const noexcept {
  if (m_config == nullptr) {
    return PanelPlacement::Attached;
  }
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    if (it->second.getBool("detached_panel", TrayWidget::Options{}.detachedPanel)) {
      return PanelPlacement::Floating;
    }
  }
  return PanelPlacement::Attached;
}

std::optional<float> TrayDrawerPanel::currentDrawerItemSize() const {
  if (m_config == nullptr) {
    return std::nullopt;
  }
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    if (it->second.hasSetting("drawer_item_size")) {
      return static_cast<float>(it->second.getDouble("drawer_item_size", TrayWidget::Options{}.drawerItemSize));
    }
  }
  return std::nullopt;
}

float TrayDrawerPanel::resolvedItemGap() const {
  float gap = Style::spaceXs;
  bool matchAdjacent = false;
  const WidgetConfig* trayConf = nullptr;
  if (m_config != nullptr) {
    if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
      trayConf = &it->second;
      matchAdjacent = trayConf->getBool("match_adjacent_spacing", TrayWidget::Options{}.matchAdjacentSpacing);
    }
    if (!m_config->config().bars.empty()) {
      gap = static_cast<float>(m_config->config().bars.front().widgetSpacing);
    }
  }
  if (!matchAdjacent || m_config == nullptr || m_config->config().bars.empty()) {
    return gap;
  }
  const auto cap = resolveWidgetBarCapsuleSpec(m_config->config().bars.front(), trayConf);
  const float pad = cap.enabled ? cap.padding * contentScale() : 0.0f;
  return gap + 2.0f * pad;
}

float TrayDrawerPanel::preferredWidth() const {
  const float itemSize = scaled(currentDrawerItemSize().value_or(Style::baseGlyphSize));
  const float gap = resolvedItemGap();
  const std::size_t drawerColumns = currentDrawerColumns();
  const std::size_t cols = std::min<std::size_t>(drawerColumns, std::max<std::size_t>(1, visibleItemCount()));
  const float contentWidth = static_cast<float>(cols) * itemSize + static_cast<float>(cols > 1 ? cols - 1 : 0) * gap;
  const float panelPadding = scaled(Style::panelPadding) * 2.0f;
  return contentWidth + panelPadding;
}

float TrayDrawerPanel::preferredHeight() const {
  const float itemSize = scaled(currentDrawerItemSize().value_or(Style::baseGlyphSize));
  const float gap = resolvedItemGap();
  const std::size_t count = std::max<std::size_t>(1, visibleItemCount());
  const std::size_t drawerColumns = currentDrawerColumns();
  const std::size_t rows = (count + drawerColumns - 1U) / drawerColumns;
  const float contentHeight = static_cast<float>(rows) * itemSize + static_cast<float>(rows > 1 ? rows - 1 : 0) * gap;
  const float panelPadding = scaled(Style::panelPadding) * 2.0f;
  return contentHeight + panelPadding;
}

void TrayDrawerPanel::create() {
  if (m_config == nullptr) {
    return;
  }
  const WidgetConfig* trayConf = nullptr;
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    trayConf = &it->second;
  }

  float widgetSpacing = Style::spaceXs;
  std::string barPosition = "top";
  if (!m_config->config().bars.empty()) {
    const auto& barConfig = m_config->config().bars.front();
    widgetSpacing = static_cast<float>(barConfig.widgetSpacing);
    barPosition = barConfig.position;
  }

  auto options = trayWidgetDefinition().resolve(
      trayConf, "tray",
      TrayWidgetDefinitionContext{
          .barPosition = barPosition,
          .inlineEntryGap = widgetSpacing,
      }
  );
  options.hiddenItems = currentHiddenItems();
  options.pinnedItems = currentPinnedItems();
  options.drawerMode = false;
  options.itemActivated = []() { PanelManager::instance().close(); };
  options.panelGridMode = true;
  options.panelGridColumns = currentDrawerColumns();
  options.customItemSize = currentDrawerItemSize();

  m_drawerWidget = std::make_unique<TrayWidget>(*m_config, m_tray, std::move(options));
  m_drawerWidget->setContentScale(contentScale());
  if (!m_config->config().bars.empty()) {
    const auto& barConfig = m_config->config().bars.front();
    m_drawerWidget->setBarCapsuleSpec(resolveWidgetBarCapsuleSpec(barConfig, trayConf));
  }
  m_drawerWidget->setAnimationManager(m_animations);
  m_drawerWidget->setUpdateCallback([]() { PanelManager::instance().requestUpdateOnly(); });
  m_drawerWidget->setRedrawCallback([]() { PanelManager::instance().requestRedraw(); });
  m_drawerWidget->setFrameTickRequestCallback([]() { PanelManager::instance().requestFrameTick(); });
  m_drawerWidget->setPanelToggleCallback([](std::string_view id, std::string_view context, std::optional<float> ax,
                                            std::optional<float> ay, Widget::PanelActivation activation) {
    PanelOpenRequest request{
        .anchorX = ax.has_value() ? std::round(*ax) : 0.0f,
        .anchorY = ay.has_value() ? std::round(*ay) : 0.0f,
        .hasAnchorPosition = ax.has_value() && ay.has_value(),
        .context = std::string(context),
    };
    if (activation == Widget::PanelActivation::Open) {
      PanelManager::instance().openPanel(std::string(id), request);
    } else {
      PanelManager::instance().togglePanel(std::string(id), request);
    }
  });
  m_drawerWidget->create();
  setRoot(m_drawerWidget->releaseRoot());
}

void TrayDrawerPanel::onClose() {
  m_drawerWidget.reset();
  clearReleasedRoot();
}

void TrayDrawerPanel::setAnimationManager(AnimationManager* mgr) noexcept {
  Panel::setAnimationManager(mgr);
  if (m_drawerWidget != nullptr) {
    m_drawerWidget->setAnimationManager(mgr);
  }
}

void TrayDrawerPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_drawerWidget == nullptr || m_drawerWidget->root() == nullptr) {
    return;
  }
  m_drawerWidget->layout(renderer, width, height);
}

void TrayDrawerPanel::doUpdate(Renderer& renderer) {
  if (m_drawerWidget == nullptr || m_drawerWidget->root() == nullptr) {
    return;
  }
  m_drawerWidget->update(renderer);
}

std::size_t TrayDrawerPanel::currentDrawerColumns() const {
  const WidgetConfig* trayConfig = nullptr;
  if (m_config != nullptr) {
    if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
      trayConfig = &it->second;
    }
  }
  // The definition owns the default and the valid range, and resolve() clamps to it.
  return trayWidgetDefinition().resolve(trayConfig, "tray", TrayWidgetDefinitionContext{}).panelGridColumns;
}

std::size_t TrayDrawerPanel::visibleItemCount() const {
  if (m_tray == nullptr) {
    return 0;
  }
  const auto hiddenItems = currentHiddenItems();
  const auto pinnedItems = currentPinnedItems();
  std::vector<std::string> hiddenLower;
  hiddenLower.reserve(hiddenItems.size());
  for (const auto& v : hiddenItems) {
    hiddenLower.push_back(StringUtils::toLower(v));
  }
  std::vector<std::string> pinnedLower;
  pinnedLower.reserve(pinnedItems.size());
  for (const auto& v : pinnedItems) {
    pinnedLower.push_back(StringUtils::toLower(v));
  }
  auto hasVariant = [&](std::string_view token, std::string_view value) {
    std::string raw(value);
    if (raw.empty()) {
      return false;
    }
    std::vector<std::string> variants;
    variants.push_back(raw);
    variants.push_back(StringUtils::toLower(raw));
    if (const auto slash = raw.find_last_of('/'); slash != std::string::npos && slash + 1 < raw.size()) {
      variants.push_back(raw.substr(slash + 1));
      variants.push_back(StringUtils::toLower(raw.substr(slash + 1)));
    }
    return std::ranges::contains(variants, token);
  };
  auto tokenMatches = [&](std::string_view token, const TrayItemInfo& item) {
    if (token.empty()) {
      return false;
    }
    const auto lowered = StringUtils::toLower(token);
    return hasVariant(lowered, item.id)
        || hasVariant(lowered, item.busName)
        || hasVariant(lowered, item.itemName)
        || hasVariant(lowered, item.processName)
        || hasVariant(lowered, item.objectPath)
        || hasVariant(lowered, item.iconName)
        || hasVariant(lowered, item.overlayIconName)
        || hasVariant(lowered, item.attentionIconName);
  };
  std::size_t visible = 0;
  for (const auto& item : m_tray->items()) {
    const bool hidden =
        std::ranges::any_of(hiddenLower, [&](const std::string& token) { return tokenMatches(token, item); });
    const bool pinned =
        std::ranges::any_of(pinnedLower, [&](const std::string& token) { return tray::tokenMatchesItem(token, item); });
    if (!hidden && !pinned) {
      ++visible;
    }
  }
  return visible;
}

std::vector<std::string> TrayDrawerPanel::currentHiddenItems() const {
  if (m_config == nullptr) {
    return {};
  }
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    return it->second.getStringList("hidden");
  }
  return {};
}

std::vector<std::string> TrayDrawerPanel::currentPinnedItems() const {
  if (m_config == nullptr) {
    return {};
  }
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    return it->second.getStringList("pinned");
  }
  return {};
}
