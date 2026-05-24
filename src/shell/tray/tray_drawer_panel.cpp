#include "shell/tray/tray_drawer_panel.h"

#include "config/config_service.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/panel/panel_manager.h"
#include "shell/tray/tray_identifier.h"
#include "util/string_utils.h"

#include <algorithm>
#include <vector>

TrayDrawerPanel::TrayDrawerPanel(TrayService* tray, ConfigService* config, std::size_t drawerColumns)
    : m_tray(tray), m_config(config), m_drawerColumns(std::clamp<std::size_t>(drawerColumns, 1U, 5U)) {}

TrayDrawerPanel::~TrayDrawerPanel() = default;

float TrayDrawerPanel::preferredWidth() const {
  const float itemSize = scaled(Style::barGlyphSize);
  const float gap = scaled(Style::spaceXs);
  const std::size_t drawerColumns = currentDrawerColumns();
  const std::size_t cols = std::min<std::size_t>(drawerColumns, std::max<std::size_t>(1, visibleItemCount()));
  const float contentWidth = static_cast<float>(cols) * itemSize + static_cast<float>(cols > 1 ? cols - 1 : 0) * gap;
  const float panelPadding = scaled(Style::panelPadding) * 2.0f;
  return contentWidth + panelPadding;
}

float TrayDrawerPanel::preferredHeight() const {
  const float itemSize = scaled(Style::barGlyphSize);
  const float gap = scaled(Style::spaceXs);
  const std::size_t count = std::max<std::size_t>(1, visibleItemCount());
  const std::size_t drawerColumns = currentDrawerColumns();
  const std::size_t rows = (count + drawerColumns - 1U) / drawerColumns;
  const float contentHeight = static_cast<float>(rows) * itemSize + static_cast<float>(rows > 1 ? rows - 1 : 0) * gap;
  const float panelPadding = scaled(Style::panelPadding) * 2.0f;
  return contentHeight + panelPadding;
}

void TrayDrawerPanel::create() {
  const auto hiddenItems = currentHiddenItems();
  const auto pinnedItems = currentPinnedItems();
  const std::size_t drawerColumns = currentDrawerColumns();
  m_drawerWidget = std::make_unique<TrayWidget>(
      m_tray, hiddenItems, pinnedItems, false, []() { PanelManager::instance().close(); }, "top", true, drawerColumns
  );
  m_drawerWidget->setContentScale(contentScale());
  m_drawerWidget->create();
  setRoot(m_drawerWidget->releaseRoot());
}

void TrayDrawerPanel::onClose() {
  m_drawerWidget.reset();
  clearReleasedRoot();
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
  if (m_config == nullptr) {
    return m_drawerColumns;
  }
  if (const auto it = m_config->config().widgets.find("tray"); it != m_config->config().widgets.end()) {
    return static_cast<std::size_t>(std::clamp<std::int64_t>(it->second.getInt("drawer_columns", 3), 1, 5));
  }
  return m_drawerColumns;
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
    return std::ranges::find(variants, std::string(token)) != variants.end();
  };
  auto tokenMatches = [&](std::string_view token, const TrayItemInfo& item) {
    if (token.empty()) {
      return false;
    }
    const auto lowered = StringUtils::toLower(token);
    return hasVariant(lowered, item.id) || hasVariant(lowered, item.busName) || hasVariant(lowered, item.itemName) ||
           hasVariant(lowered, item.processName) || hasVariant(lowered, item.objectPath) ||
           hasVariant(lowered, item.iconName) || hasVariant(lowered, item.overlayIconName) ||
           hasVariant(lowered, item.attentionIconName);
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
