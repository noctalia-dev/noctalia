#pragma once

#include "dbus/tray/tray_service.h"
#include "shell/bar/widget.h"
#include "system/icon_resolver.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class ConfigService;
class Flex;
class Image;
class InputArea;
class Glyph;
class Box;

enum class TrayIconType : std::uint8_t { Default, Desktop, Symbolic };

class TrayWidget : public Widget {
public:
  struct Options {
    std::vector<std::string> hiddenItems;
    std::vector<std::string> pinnedItems;
    bool hidePassive = true;
    bool colorizeIcons = false;
    bool drawerMode = false;
    std::string glyph;
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool peek = false;
    int peekDelayMs = kBarRevealDelayMs;
    std::function<void()> itemActivated;
    std::string barPosition = "top";
    bool panelGridMode = false;
    std::size_t panelGridColumns = 3;
    float inlineEntryGap = Style::spaceXs;
    bool matchAdjacentSpacing = false;
    std::optional<float> customItemSize;
    // Read by TrayDrawerPanel, not by TrayWidget: they live here so the tray widget definition owns their defaults.
    double drawerItemSize = Style::baseGlyphSize;
    bool detachedPanel = false;
    std::vector<std::string> iconOverrideBlacklist;
    TrayIconType iconType = TrayIconType::Default;
    double drawerBackgroundOpacity = 1.0;
  };

  TrayWidget(ConfigService& config, TrayService* tray, Options options);
  ~TrayWidget() override;

  void setHoverOverlayParent(Node* node) noexcept { m_hoverOverlayParent = node; }
  void setCapsuleCross(float cross) noexcept { m_capsuleCross = cross; }
  void setColorizeIcons(bool enabled);
  void create() override;
  bool wantsBarHoverHighlight() const noexcept override { return false; }

private:
  struct HoverOverlayEntry {
    InputArea* area = nullptr;
    Box* box = nullptr;
    float padding = 0.0F;
  };

  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void buildDesktopIconIndex();
  std::string resolveIconPath(const TrayItemInfo& item);
  std::string resolveDesktopIconPath(const TrayItemInfo& item);
  std::string resolveSymbolicIconPath(const TrayItemInfo& item);
  void syncState(Renderer& renderer);
  void rebuild(Renderer& renderer);
  std::string iconForItem(const TrayItemInfo& item) const;
  bool isPinnedItem(const TrayItemInfo& item) const;
  bool isHiddenItem(const TrayItemInfo& item) const;
  bool isIconOverrideBlacklisted(const TrayItemInfo& item) const;
  std::string_view resolvedGlyph() const;
  // Bar section gap is between capsule shells; inline tray icons share one shell, so add the
  // lateral inset that adjacent single-icon capsules would contribute between their icons.
  float resolvedInlineEntryGap() const;
  void refreshAppIconColorization(Renderer& renderer);
  void layoutHoverOverlays();
  void clearHoverOverlays();
  std::optional<ColorSpec> currentAppIconColorizeTint() const;

  ConfigService& m_config;
  TrayService* m_tray = nullptr;
  Flex* m_container = nullptr;
  Glyph* m_peekGlyph = nullptr;
  IconResolver m_iconResolver;
  std::unordered_map<std::string, std::string> m_appIcons;
  std::unordered_map<std::string, std::string> m_preferredIconPaths;
  std::uint64_t m_desktopEntriesVersion = 0;
  std::uint64_t m_themePathIndexListener = 0;
  std::vector<TrayItemInfo> m_items;
  std::vector<std::string> m_hiddenItems;
  std::vector<std::string> m_pinnedItems;
  bool m_hidePassive = true;
  bool m_colorizeIcons = false;
  std::vector<Image*> m_loadedImages;
  std::vector<Image*> m_colorizedAppIcons;
  // Item InputAreas by tray item id, reused across rebuilds so icon updates keep hover and press capture.
  std::unordered_map<std::string, InputArea*> m_itemAreas;
  std::unordered_map<std::string, std::size_t> m_initialPixmaps;
  std::unordered_map<std::string, bool> m_preferPixmap;
  float m_contentHeight = 0.0F;
  bool m_isVertical = false;
  bool m_rebuildPending = true;
  bool m_drawerMode = false;
  std::string m_glyph;
  GlyphPosition m_glyphPosition = GlyphPosition::Before;
  std::function<void()> m_itemActivated;
  std::string m_barPosition;
  bool m_panelGridMode = false;
  std::size_t m_panelGridColumns = 3;
  float m_inlineEntryGap = Style::spaceXs;
  bool m_matchAdjacentSpacing = false;
  std::optional<float> m_customItemSize;
  bool m_appIconColorizeDirty = false;
  InputArea* m_drawerTrigger = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
  Signal<>::ScopedConnection m_appIconColorizeConn;
  Node* m_hoverOverlayParent = nullptr;
  std::vector<HoverOverlayEntry> m_hoverOverlays;
  float m_capsuleCross = 0.0F;
  std::vector<std::string> m_iconOverrideBlacklist;
  TrayIconType m_iconType = TrayIconType::Default;
};
