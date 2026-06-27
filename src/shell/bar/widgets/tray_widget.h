#pragma once

#include "dbus/tray/tray_service.h"
#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class ConfigService;
class Flex;
class Image;
class InputArea;
class Glyph;

struct TrayWidgetOptions {
  std::vector<std::string> hiddenItems;
  std::vector<std::string> pinnedItems;
  bool drawerMode = false;
  std::function<void()> itemActivated;
  std::string barPosition = "top";
  bool panelGridMode = false;
  std::size_t panelGridColumns = 3;
  float inlineEntryGap = Style::spaceXs;
  bool matchAdjacentSpacing = false;
};

class TrayWidget : public Widget {
public:
  TrayWidget(ConfigService& config, TrayService* tray, TrayWidgetOptions options = {});

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void buildDesktopIconIndex();
  [[nodiscard]] std::string resolveIconPath(const TrayItemInfo& item);
  [[nodiscard]] std::string resolveFromTrayThemePath(std::string_view themePath, std::string_view iconName);
  void syncState(Renderer& renderer);
  void rebuild(Renderer& renderer);
  [[nodiscard]] std::string iconForItem(const TrayItemInfo& item) const;
  [[nodiscard]] bool isPinnedItem(const TrayItemInfo& item) const;
  [[nodiscard]] bool isHiddenItem(const TrayItemInfo& item) const;
  [[nodiscard]] std::string drawerChevronGlyph(bool panelOpen) const;
  // Bar section gap is between capsule shells; inline tray icons share one shell, so add the
  // lateral inset that adjacent single-icon capsules would contribute between their icons.
  [[nodiscard]] float resolvedInlineEntryGap() const;
  void refreshAppIconColorization(Renderer& renderer);
  [[nodiscard]] std::optional<ColorSpec> currentAppIconColorizeTint() const;

  ConfigService& m_config;
  TrayService* m_tray = nullptr;
  Flex* m_container = nullptr;
  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_appIcons;
  std::unordered_map<std::string, std::string> m_preferredIconPaths;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_trayThemePathIcons;
  std::uint64_t m_desktopEntriesVersion = 0;
  std::vector<TrayItemInfo> m_items;
  std::vector<std::string> m_hiddenItems;
  std::vector<std::string> m_pinnedItems;
  std::vector<Image*> m_loadedImages;
  std::vector<Image*> m_colorizedAppIcons;
  std::unordered_map<std::string, std::size_t> m_initialPixmaps;
  std::unordered_map<std::string, bool> m_preferPixmap;
  float m_contentHeight = 0.0f;
  bool m_isVertical = false;
  bool m_rebuildPending = true;
  bool m_drawerMode = false;
  std::function<void()> m_itemActivated;
  std::string m_barPosition;
  bool m_panelGridMode = false;
  std::size_t m_panelGridColumns = 3;
  float m_inlineEntryGap = Style::spaceXs;
  bool m_matchAdjacentSpacing = false;
  bool m_appIconColorizeDirty = false;
  InputArea* m_drawerTrigger = nullptr;
  Glyph* m_drawerChevron = nullptr;
  std::string m_drawerChevronGlyph;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_appIconColorizeConn;
};
