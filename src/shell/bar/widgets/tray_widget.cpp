#include "shell/bar/widgets/tray_widget.h"

#include "core/log.h"
#include "core/ui_phase.h"
#include "dbus/tray/tray_service.h"
#include "render/core/image_file_loader.h"
#include "render/core/image_source_log.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "render/text/glyph_registry.h"
#include "shell/panel/panel_manager.h"
#include "shell/tray/tray_identifier.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <memory>
#include <optional>
#include <string>

namespace {

  namespace fs = std::filesystem;

  constexpr Logger kLog("tray");

  using tray::identifierVariants;

  void addIconAlias(std::unordered_map<std::string, std::string>& index, std::string_view key, std::string_view icon) {
    if (key.empty() || icon.empty()) {
      return;
    }

    for (const auto& variant : identifierVariants(key)) {
      index.try_emplace(variant, std::string(icon));
    }
  }

  std::string execBasename(std::string_view exec) {
    if (exec.empty()) {
      return {};
    }

    std::string token;
    bool inSingle = false;
    bool inDouble = false;
    for (char c : exec) {
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        break;
      }
      token.push_back(c);
    }

    if (token.empty()) {
      return {};
    }
    if (const auto slash = token.find_last_of('/'); slash != std::string::npos && slash + 1 < token.size()) {
      token = token.substr(slash + 1);
    }
    return token;
  }

  bool isSymbolicIconName(std::string_view name) {
    return name.find("symbolic") != std::string_view::npos || name.ends_with("-panel") || name.ends_with("_panel");
  }

  bool isSymbolicIconPath(std::string_view path) {
    return path.find("symbolic") != std::string_view::npos || path.find("/status/") != std::string_view::npos;
  }

  bool isSvgPath(std::string_view path) { return path.ends_with(".svg") || path.ends_with(".SVG"); }

  std::optional<LoadedImageFile>
  loadSymbolicTrayIcon(const std::string& path, int targetSize, const Color& symbolicColor) {
    std::string loadError;
    auto loaded = loadImageFile(path, targetSize, &loadError);
    if (!loaded) {
      kLog.debug("tray widget symbolic icon decode failed path={} error={}", ImageSourceLog::describe(path), loadError);
      return std::nullopt;
    }

    auto toByte = [](float channel) -> std::uint8_t {
      const float clamped = std::clamp(channel, 0.0f, 1.0f);
      return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
    };
    const std::uint8_t rr = toByte(symbolicColor.r);
    const std::uint8_t gg = toByte(symbolicColor.g);
    const std::uint8_t bb = toByte(symbolicColor.b);

    for (std::size_t i = 0; i + 3 < loaded->rgba.size(); i += 4) {
      const std::uint8_t a = loaded->rgba[i + 3];
      if (a == 0) {
        continue;
      }
      const float lum =
          (loaded->rgba[i] * 0.299f + loaded->rgba[i + 1] * 0.587f + loaded->rgba[i + 2] * 0.114f) / 255.0f;
      loaded->rgba[i + 0] = rr;
      loaded->rgba[i + 1] = gg;
      loaded->rgba[i + 2] = bb;
      loaded->rgba[i + 3] = static_cast<std::uint8_t>(std::lround(a * lum));
    }

    return loaded;
  }

  bool isUniqueBusName(std::string_view value) { return !value.empty() && value.front() == ':'; }

} // namespace

TrayWidget::TrayWidget(
    TrayService* tray, std::vector<std::string> hiddenItems, std::vector<std::string> pinnedItems, bool drawerMode,
    std::function<void()> itemActivated, std::string barPosition, bool panelGridMode, std::size_t panelGridColumns,
    float inlineEntryGap, bool matchAdjacentSpacing
)
    : m_tray(tray), m_hiddenItems(std::move(hiddenItems)), m_pinnedItems(std::move(pinnedItems)),
      m_drawerMode(drawerMode), m_itemActivated(std::move(itemActivated)), m_barPosition(std::move(barPosition)),
      m_panelGridMode(panelGridMode), m_panelGridColumns(std::clamp<std::size_t>(panelGridColumns, 1U, 5U)),
      m_inlineEntryGap(std::max(0.0f, inlineEntryGap)), m_matchAdjacentSpacing(matchAdjacentSpacing) {
  auto normalizeTokens = [](std::vector<std::string>& tokens) {
    std::vector<std::string> normalized;
    normalized.reserve(tokens.size());
    for (const auto& token : tokens) {
      for (const auto& variant : identifierVariants(token)) {
        if (std::ranges::find(normalized, variant) == normalized.end()) {
          normalized.push_back(variant);
        }
      }
    }
    tokens = std::move(normalized);
  };
  normalizeTokens(m_hiddenItems);
  normalizeTokens(m_pinnedItems);
  buildDesktopIconIndex();
}

std::string TrayWidget::resolveFromTrayThemePath(std::string_view themePath, std::string_view iconName) {
  if (themePath.empty() || iconName.empty()) {
    return {};
  }

  const std::string themePathKey(themePath);
  auto [cacheIt, inserted] = m_trayThemePathIcons.try_emplace(themePathKey);
  if (inserted) {
    std::error_code ec;
    if (!fs::is_directory(themePathKey, ec)) {
      return {};
    }

    auto& iconIndex = cacheIt->second;
    for (fs::recursive_directory_iterator it(themePathKey, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      if (ec || !it->is_regular_file()) {
        continue;
      }

      const fs::path path = it->path();
      const auto extension = StringUtils::toLower(path.extension().string());
      if (extension != ".svg" && extension != ".png") {
        continue;
      }

      const std::string stem = path.stem().string();
      for (const auto& variant : identifierVariants(stem)) {
        iconIndex.try_emplace(variant, path.string());
      }
    }
  }

  const auto& iconIndex = cacheIt->second;
  for (const auto& variant : identifierVariants(iconName)) {
    if (const auto it = iconIndex.find(variant); it != iconIndex.end()) {
      return it->second;
    }
  }

  return {};
}

float TrayWidget::resolvedInlineEntryGap() const {
  if (!m_matchAdjacentSpacing) {
    return m_inlineEntryGap;
  }
  const auto& cap = barCapsuleSpec();
  const float pad = cap.enabled ? cap.padding * m_contentScale : 0.0f;
  return m_inlineEntryGap + 2.0f * pad;
}

void TrayWidget::create() {
  auto container = ui::makeFlex(
      m_panelGridMode ? FlexDirection::Vertical : FlexDirection::Horizontal,
      {
          .out = &m_container,
          .align = m_panelGridMode ? FlexAlign::Start : FlexAlign::Center,
          .justify = m_panelGridMode ? std::optional<FlexJustify>{} : std::optional<FlexJustify>{FlexJustify::Start},
          .gap = m_panelGridMode ? Style::spaceXs * m_contentScale : resolvedInlineEntryGap(),
      }
  );

  setRoot(std::move(container));
}

void TrayWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_container == nullptr) {
    return;
  }
  if (m_drawerMode && m_drawerChevron != nullptr && m_drawerTrigger != nullptr) {
    const bool panelOpen = PanelManager::instance().isOpenPanel("tray-drawer");
    const std::string glyphName = drawerChevronGlyph(panelOpen);
    if (m_drawerChevronGlyph != glyphName) {
      m_drawerChevronGlyph = glyphName;
      m_drawerChevron->setGlyph(glyphName);
      m_drawerChevron->setGlyphSize(m_drawerTrigger->width());
      m_drawerChevron->measure(renderer);
      m_drawerChevron->setPosition(
          std::round((m_drawerTrigger->width() - m_drawerChevron->width()) * 0.5f),
          std::round((m_drawerTrigger->height() - m_drawerChevron->height()) * 0.5f)
      );
      requestRedraw();
    }
  }
  if (m_panelGridMode) {
    syncState(renderer);
    if (m_rebuildPending) {
      rebuild(renderer);
      m_rebuildPending = false;
    }
    m_container->layout(renderer);
    return;
  }
  const bool vertical = containerHeight > containerWidth;
  if (vertical != m_isVertical) {
    m_isVertical = vertical;
    m_container->setDirection(m_isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal);
  }
  syncState(renderer);
  if (containerHeight > 0.0f && std::abs(containerHeight - m_contentHeight) > 0.5f) {
    m_contentHeight = containerHeight;
    m_rebuildPending = true;
  }
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  }

  if (!m_panelGridMode) {
    m_container->setGap(resolvedInlineEntryGap());
  }
  m_container->layout(renderer);
}

void TrayWidget::doUpdate(Renderer& renderer) {
  syncState(renderer);
  if (m_rebuildPending) {
    rebuild(renderer);
    m_rebuildPending = false;
  }
}

void TrayWidget::syncState(Renderer& renderer) {
  (void)renderer;
  const auto desktopVersion = desktopEntriesVersion();
  const bool desktopEntriesChanged = desktopVersion != m_desktopEntriesVersion;
  if (desktopEntriesChanged) {
    buildDesktopIconIndex();
    m_preferredIconPaths.clear();
  }

  const auto next_items = (m_tray != nullptr) ? m_tray->items() : std::vector<TrayItemInfo>{};
  if (!desktopEntriesChanged && next_items == m_items) {
    return;
  }

  std::unordered_map<std::string, const TrayItemInfo*> previousById;
  previousById.reserve(m_items.size());
  for (const auto& oldItem : m_items) {
    previousById[oldItem.id] = &oldItem;
  }

  std::unordered_map<std::string, bool> stillPresent;
  stillPresent.reserve(next_items.size());
  for (const auto& item : next_items) {
    stillPresent[item.id] = true;

    auto hashVec = [](const std::vector<std::uint8_t>& vec) -> std::size_t {
      if (vec.empty()) {
        return 0;
      }
      return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(vec.data()), vec.size()));
    };
    const std::size_t currentHash = hashVec(item.iconArgb32) ^ (hashVec(item.attentionArgb32) << 1);

    if (!m_initialPixmaps.contains(item.id)) {
      m_initialPixmaps[item.id] = currentHash;
    } else if (m_initialPixmaps[item.id] != currentHash) {
      m_preferPixmap[item.id] = true;
    }

    const auto prevIt = previousById.find(item.id);
    if (prevIt == previousById.end() || prevIt->second == nullptr) {
      continue;
    }
    const TrayItemInfo& prev = *prevIt->second;

    // Resolved icon path cache is keyed by item id. Invalidate when icon-relevant
    // metadata changes, or we can keep showing stale paths after icon switches.
    if (prev.iconName != item.iconName || prev.overlayIconName != item.overlayIconName ||
        prev.attentionIconName != item.attentionIconName || prev.iconThemePath != item.iconThemePath ||
        prev.needsAttention != item.needsAttention || prev.status != item.status ||
        prev.overlayWidth != item.overlayWidth || prev.overlayHeight != item.overlayHeight ||
        prev.overlayArgb32 != item.overlayArgb32 || prev.iconWidth != item.iconWidth ||
        prev.iconHeight != item.iconHeight || prev.iconArgb32 != item.iconArgb32 ||
        prev.attentionWidth != item.attentionWidth || prev.attentionHeight != item.attentionHeight ||
        prev.attentionArgb32 != item.attentionArgb32) {
      kLog.debug(
          "tray widget invalidate icon cache id={} icon='{}'->'{}' overlay='{}'->'{}' attention='{}'->'{}' "
          "status={}=>{}",
          item.id, prev.iconName, item.iconName, prev.overlayIconName, item.overlayIconName, prev.attentionIconName,
          item.attentionIconName, prev.status, item.status
      );
      m_preferredIconPaths.erase(item.id);
    }
  }
  for (auto it = m_preferredIconPaths.begin(); it != m_preferredIconPaths.end();) {
    if (!stillPresent.contains(it->first)) {
      it = m_preferredIconPaths.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = m_initialPixmaps.begin(); it != m_initialPixmaps.end();) {
    if (!stillPresent.contains(it->first)) {
      it = m_initialPixmaps.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = m_preferPixmap.begin(); it != m_preferPixmap.end();) {
    if (!stillPresent.contains(it->first)) {
      it = m_preferPixmap.erase(it);
    } else {
      ++it;
    }
  }

  m_items = next_items;
  m_rebuildPending = true;
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
  requestRedraw();
}

void TrayWidget::rebuild(Renderer& renderer) {
  uiAssertNotRendering("TrayWidget::rebuild");
  if (m_container == nullptr) {
    return;
  }

  for (auto* image : m_loadedImages) {
    if (image != nullptr) {
      image->clear(renderer);
    }
  }
  m_loadedImages.clear();

  while (!m_container->children().empty()) {
    m_container->removeChild(m_container->children().back().get());
  }

  if (m_drawerMode) {
    m_drawerTrigger = nullptr;
    m_drawerChevron = nullptr;
    m_drawerChevronGlyph.clear();
    bool hasDrawerItems = false;
    for (const auto& item : m_items) {
      if (isHiddenItem(item) || isPinnedItem(item)) {
        continue;
      }
      hasDrawerItems = true;
      break;
    }
    if (hasDrawerItems) {
      const float itemSize = Style::barGlyphSize * m_contentScale;
      auto triggerArea = std::make_unique<InputArea>();
      auto* triggerPtr = triggerArea.get();
      m_drawerTrigger = triggerPtr;
      triggerArea->setSize(itemSize, itemSize);
      triggerArea->setOnClick([this, triggerPtr](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) {
          float ax = 0.0f;
          float ay = 0.0f;
          Node::absolutePosition(triggerPtr, ax, ay);
          // Open below / away from the bar edge relative to the tray button center.
          const float centerX = ax + triggerPtr->width() * 0.5f;
          const float centerY = ay + triggerPtr->height() * 0.5f;
          float anchorX = centerX;
          float anchorY = centerY;
          if (m_barPosition == "top") {
            anchorY += triggerPtr->height() * 0.5f + Style::spaceXs * m_contentScale;
          } else if (m_barPosition == "bottom") {
            anchorY -= triggerPtr->height() * 0.5f + Style::spaceXs * m_contentScale;
          } else if (m_barPosition == "left") {
            anchorX += triggerPtr->width() * 0.5f + Style::spaceXs * m_contentScale;
          } else if (m_barPosition == "right") {
            anchorX -= triggerPtr->width() * 0.5f + Style::spaceXs * m_contentScale;
          }
          requestPanelToggle("tray-drawer", {}, anchorX, anchorY);
        }
      });
      const bool panelOpen = PanelManager::instance().isOpenPanel("tray-drawer");
      m_drawerChevronGlyph = drawerChevronGlyph(panelOpen);
      auto glyph = ui::glyph({
          .glyph = m_drawerChevronGlyph,
          .glyphSize = itemSize,
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      });
      glyph->measure(renderer);
      glyph->setPosition(
          std::round((itemSize - glyph->width()) * 0.5f), std::round((itemSize - glyph->height()) * 0.5f)
      );
      m_drawerChevron = glyph.get();
      triggerArea->addChild(std::move(glyph));
      m_container->addChild(std::move(triggerArea));
    }
  }

  Flex* gridRow = nullptr;
  std::size_t gridCol = 0;
  for (const auto& item : m_items) {
    if (isHiddenItem(item)) {
      continue;
    }
    if (m_drawerMode && !isPinnedItem(item)) {
      continue;
    }
    if (m_panelGridMode && isPinnedItem(item)) {
      continue;
    }
    const std::string iconPath = resolveIconPath(item);
    const float itemSize = Style::barGlyphSize * m_contentScale;
    const float iconSize = itemSize;
    const int iconRequestSize = std::max(32, static_cast<int>(std::round(iconSize * 2.0f)));

    std::unique_ptr<Node> iconNode;
    float iconW = iconSize;
    float iconH = iconSize;

    if (!iconPath.empty()) {
      auto image = ui::image({
          .fit = ImageFit::Contain,
          .width = iconSize,
          .height = iconSize,
      });
      bool loadedFromFile = false;
      const bool symbolicPath = isSymbolicIconPath(iconPath);
      const Color symbolicColor = resolveColorSpec(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
      if (symbolicPath && isSvgPath(iconPath)) {
        if (auto symbolic = loadSymbolicTrayIcon(iconPath, iconRequestSize, symbolicColor)) {
          loadedFromFile = image->setSourceRaw(
              renderer, symbolic->rgba.data(), symbolic->rgba.size(), symbolic->width, symbolic->height, 0,
              PixmapFormat::RGBA, true
          );
        }
      }
      if (!loadedFromFile) {
        loadedFromFile = image->setSourceFile(renderer, iconPath, iconRequestSize, true);
      }

      if (loadedFromFile) {
        if (symbolicPath && !isSvgPath(iconPath)) {
          image->setTint(symbolicColor);
        }
        iconW = iconSize;
        iconH = iconSize;
        m_loadedImages.push_back(image.get());
        iconNode = std::move(image);
      } else {
        kLog.debug("tray widget icon id={} source=file path={} failed-to-load", item.id, iconPath);
      }
    }

    if (iconNode == nullptr) {
      const auto& pixmap =
          item.needsAttention && !item.attentionArgb32.empty() ? item.attentionArgb32 : item.iconArgb32;
      const std::int32_t pixmapW =
          item.needsAttention && !item.attentionArgb32.empty() ? item.attentionWidth : item.iconWidth;
      const std::int32_t pixmapH =
          item.needsAttention && !item.attentionArgb32.empty() ? item.attentionHeight : item.iconHeight;

      if (!pixmap.empty() && pixmapW > 0 && pixmapH > 0) {
        auto image = ui::image({
            .fit = ImageFit::Contain,
            .width = iconSize,
            .height = iconSize,
        });
        if (image->setSourceRaw(
                renderer, pixmap.data(), pixmap.size(), pixmapW, pixmapH, 0, PixmapFormat::ARGB, true
            )) {
          iconW = iconSize;
          iconH = iconSize;
          m_loadedImages.push_back(image.get());
          iconNode = std::move(image);
        } else {
          kLog.debug("tray widget icon id={} source=pixmap size={}x{} failed-to-load", item.id, pixmapW, pixmapH);
        }
      }
    }

    std::unique_ptr<Node> overlayNode;
    float overlayW = iconSize;
    float overlayH = iconSize;
    if (!item.needsAttention) {
      auto resolveOverlayPath = [this, &item, iconRequestSize](const std::string& overlayName) -> std::string {
        if (overlayName.empty()) {
          return {};
        }
        if (const auto themed = resolveFromTrayThemePath(item.iconThemePath, overlayName); !themed.empty()) {
          return themed;
        }
        if (const auto direct = m_iconResolver.resolve(overlayName, iconRequestSize); !direct.empty()) {
          return direct;
        }
        if (const auto it = m_appIcons.find(overlayName); it != m_appIcons.end()) {
          return m_iconResolver.resolve(it->second, iconRequestSize);
        }
        const std::string lower = StringUtils::toLower(overlayName);
        if (const auto it = m_appIcons.find(lower); it != m_appIcons.end()) {
          return m_iconResolver.resolve(it->second, iconRequestSize);
        }
        return {};
      };

      const std::string overlayPath = resolveOverlayPath(item.overlayIconName);
      if (!overlayPath.empty()) {
        auto overlayImage = ui::image({
            .fit = ImageFit::Contain,
            .width = iconSize,
            .height = iconSize,
        });
        if (overlayImage->setSourceFile(renderer, overlayPath, iconRequestSize, true)) {
          overlayW = iconSize;
          overlayH = iconSize;
          m_loadedImages.push_back(overlayImage.get());
          overlayNode = std::move(overlayImage);
        }
      }

      if (overlayNode == nullptr && !item.overlayArgb32.empty() && item.overlayWidth > 0 && item.overlayHeight > 0) {
        auto overlayImage = ui::image({
            .fit = ImageFit::Contain,
            .width = iconSize,
            .height = iconSize,
        });
        if (overlayImage->setSourceRaw(
                renderer, item.overlayArgb32.data(), item.overlayArgb32.size(), item.overlayWidth, item.overlayHeight,
                0, PixmapFormat::ARGB, true
            )) {
          overlayW = iconSize;
          overlayH = iconSize;
          m_loadedImages.push_back(overlayImage.get());
          overlayNode = std::move(overlayImage);
        }
      }
    }

    if (iconNode == nullptr) {
      const std::string fallback = iconForItem(item);
      auto glyph = ui::glyph({
          .glyph = fallback,
          .glyphSize = iconSize,
          .color = item.needsAttention ? colorSpecFromRole(ColorRole::Error)
                                       : widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
      });
      glyph->measure(renderer);
      iconW = glyph->width();
      iconH = glyph->height();
      iconNode = std::move(glyph);
    }

    if (overlayNode != nullptr && iconNode != nullptr) {
      auto stack = std::make_unique<Node>();
      stack->setSize(itemSize, itemSize);
      iconNode->setPosition(std::round((itemSize - iconW) * 0.5f), std::round((itemSize - iconH) * 0.5f));
      overlayNode->setPosition(std::round((itemSize - overlayW) * 0.5f), std::round((itemSize - overlayH) * 0.5f));
      stack->addChild(std::move(iconNode));
      stack->addChild(std::move(overlayNode));
      iconNode = std::move(stack);
      iconW = itemSize;
      iconH = itemSize;
    }

    // Wrap icon in InputArea for click handling
    auto area = std::make_unique<InputArea>();
    area->setSize(itemSize, itemSize);
    iconNode->setPosition(std::round((itemSize - iconW) * 0.5f), std::round((itemSize - iconH) * 0.5f));
    auto itemId = item.id;
    area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
    area->setOnClick([this, itemId](const InputArea::PointerData& data) {
      if (m_tray == nullptr) {
        return;
      }
      if (data.button == BTN_LEFT) {
        (void)m_tray->activateItem(itemId);
        if (m_itemActivated) {
          m_itemActivated();
        }
      } else if (data.button == BTN_RIGHT) {
        m_tray->requestMenuToggle(itemId);
      }
    });
    area->addChild(std::move(iconNode));

    if (const std::string tooltipText = tray::formatTrayItemTooltip(item); !tooltipText.empty()) {
      area->setTooltip(tooltipText);
    }

    if (m_panelGridMode) {
      if (gridRow == nullptr || gridCol >= m_panelGridColumns) {
        gridRow = static_cast<Flex*>(m_container->addChild(
            ui::row({
                .align = FlexAlign::Center,
                .gap = Style::spaceXs * m_contentScale,
            })
        ));
        gridCol = 0;
      }
      gridRow->addChild(std::move(area));
      ++gridCol;
    } else {
      m_container->addChild(std::move(area));
    }
  }

  m_container->setVisible(!m_container->children().empty());
}

std::string TrayWidget::drawerChevronGlyph(bool panelOpen) const {
  if (m_barPosition == "bottom") {
    return panelOpen ? "chevron-down" : "chevron-up";
  }
  if (m_barPosition == "left") {
    return panelOpen ? "chevron-left" : "chevron-right";
  }
  if (m_barPosition == "right") {
    return panelOpen ? "chevron-right" : "chevron-left";
  }
  return panelOpen ? "chevron-up" : "chevron-down";
}

bool TrayWidget::isHiddenItem(const TrayItemInfo& item) const {
  if (m_hiddenItems.empty()) {
    return false;
  }

  std::vector<std::string> candidates;
  auto appendVariants = [&candidates](std::string_view text) {
    for (const auto& variant : identifierVariants(text)) {
      if (std::ranges::find(candidates, variant) == candidates.end()) {
        candidates.push_back(variant);
      }
    }
  };

  appendVariants(item.id);
  appendVariants(item.busName);
  appendVariants(item.objectPath);
  appendVariants(item.itemName);
  appendVariants(item.processName);
  appendVariants(item.title);
  appendVariants(item.iconName);
  appendVariants(item.attentionIconName);

  for (const auto& needle : m_hiddenItems) {
    if (std::ranges::find(candidates, needle) != candidates.end()) {
      return true;
    }
  }

  return false;
}

bool TrayWidget::isPinnedItem(const TrayItemInfo& item) const {
  if (m_pinnedItems.empty()) {
    return false;
  }

  for (const auto& needle : m_pinnedItems) {
    if (tray::tokenMatchesItem(needle, item)) {
      return true;
    }
  }

  return false;
}

void TrayWidget::buildDesktopIconIndex() {
  m_appIcons.clear();
  const auto& entries = desktopEntries();
  for (const auto& entry : entries) {
    if (entry.id.empty()) {
      continue;
    }

    if (!entry.icon.empty()) {
      addIconAlias(m_appIcons, entry.id, entry.icon);
      addIconAlias(m_appIcons, entry.name, entry.icon);
      addIconAlias(m_appIcons, entry.nameLower, entry.icon);
      addIconAlias(m_appIcons, entry.icon, entry.icon);
      addIconAlias(m_appIcons, execBasename(entry.exec), entry.icon);
      addIconAlias(m_appIcons, entry.startupWmClass, entry.icon);
    }
  }
  m_desktopEntriesVersion = desktopEntriesVersion();
}

std::string TrayWidget::resolveIconPath(const TrayItemInfo& item) {
  const bool hasNamedIcon = item.needsAttention ? !item.attentionIconName.empty() : !item.iconName.empty();
  if (const auto it = m_preferPixmap.find(item.id); it != m_preferPixmap.end() && it->second) {
    // Some indicators publish pixmaps late (after startup) that are monochrome
    // or stale. Keep using named-icon resolution when available.
    if (!hasNamedIcon) {
      return {};
    }
  }

  if (const auto it = m_preferredIconPaths.find(item.id); it != m_preferredIconPaths.end() && !it->second.empty()) {
    return it->second;
  }

  std::string preferred;
  if (item.needsAttention && !item.attentionIconName.empty()) {
    preferred = item.attentionIconName;
  } else if (item.needsAttention && !item.attentionArgb32.empty()) {
    preferred = "";
  } else {
    preferred = item.iconName;
  }
  if (tray::looksGenericStatusItemName(preferred)) {
    preferred.clear();
  }

  if (const auto themed = resolveFromTrayThemePath(item.iconThemePath, preferred); !themed.empty()) {
    m_preferredIconPaths[item.id] = themed;
    return themed;
  }

  // Match the on-screen request size used when the icon is loaded (see rebuild).
  const int iconTargetSize = std::max(32, static_cast<int>(std::round(Style::barGlyphSize * m_contentScale * 2.0f)));

  auto resolveMapped = [this, iconTargetSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }

    if (const auto it = m_appIcons.find(name); it != m_appIcons.end()) {
      if (const auto mapped = m_iconResolver.resolve(it->second, iconTargetSize); !mapped.empty()) {
        return mapped;
      }
    }

    const std::string lower = StringUtils::toLower(name);
    if (const auto it = m_appIcons.find(lower); it != m_appIcons.end()) {
      if (const auto mapped = m_iconResolver.resolve(it->second, iconTargetSize); !mapped.empty()) {
        return mapped;
      }
    }

    if (const auto dot = name.rfind('.'); dot != std::string::npos && dot + 1 < name.size()) {
      const auto tail = name.substr(dot + 1);
      if (const auto it = m_appIcons.find(tail); it != m_appIcons.end()) {
        if (const auto mapped = m_iconResolver.resolve(it->second, iconTargetSize); !mapped.empty()) {
          return mapped;
        }
      }
      const std::string tailLower = StringUtils::toLower(tail);
      if (const auto it = m_appIcons.find(tailLower); it != m_appIcons.end()) {
        if (const auto mapped = m_iconResolver.resolve(it->second, iconTargetSize); !mapped.empty()) {
          return mapped;
        }
      }
    }

    return {};
  };

  auto resolveDirect = [this, iconTargetSize](const std::string& name) -> std::string {
    if (name.empty()) {
      return {};
    }
    return m_iconResolver.resolve(name, iconTargetSize);
  };

  std::string symbolicFallback;

  const std::string stableBusName = isUniqueBusName(item.busName) ? std::string{} : item.busName;
  const std::string stableItemId = (!item.id.empty() && !isUniqueBusName(item.id))
                                       ? item.id
                                       : (isUniqueBusName(item.busName) ? item.objectPath : item.id);

  std::vector<std::pair<const char*, const std::string*>> candidates;
  candidates.reserve(12);
  candidates.emplace_back("preferred", &preferred);
  // When an explicit tray IconName is provided, treat it as authoritative.
  // Falling back to generic app-id/title mappings can hide stateful icon
  // changes (e.g. indicator on/off variants) behind a constant app icon.
  const bool hasTargetPixmap = item.needsAttention ? !item.attentionArgb32.empty() : !item.iconArgb32.empty();
  if (preferred.empty() && !hasTargetPixmap) {
    candidates.emplace_back("itemName", &item.itemName);
    candidates.emplace_back("processName", &item.processName);
    candidates.emplace_back("title", &item.title);
    candidates.emplace_back("objectPath", &item.objectPath);
    candidates.emplace_back("busName", &stableBusName);
    candidates.emplace_back("id", &stableItemId);
  }

  for (const auto& [_, candidate] : candidates) {
    for (const auto& variant : identifierVariants(*candidate)) {
      if (tray::looksGenericStatusItemName(variant)) {
        continue;
      }

      if (const auto mapped = resolveMapped(variant); !mapped.empty()) {
        if (!isSymbolicIconPath(mapped)) {
          m_preferredIconPaths[item.id] = mapped;
          return mapped;
        }
        if (symbolicFallback.empty()) {
          symbolicFallback = mapped;
        }
      }

      if (const auto direct = resolveDirect(variant); !direct.empty()) {
        if (!isSymbolicIconName(variant) && !isSymbolicIconPath(direct)) {
          m_preferredIconPaths[item.id] = direct;
          return direct;
        }
        if (symbolicFallback.empty()) {
          symbolicFallback = direct;
        }
      }
    }
  }

  kLog.debug(
      "tray widget resolve id={} fallback={} preferred='{}' itemName='{}' title='{}' bus='{}' objectPath='{}' "
      "stableBus='{}' stableId='{}'",
      item.id, symbolicFallback, preferred, item.itemName, item.title, item.busName, item.objectPath, stableBusName,
      stableItemId
  );
  return symbolicFallback;
}

std::string TrayWidget::iconForItem(const TrayItemInfo& item) const {
  const std::string preferred =
      item.needsAttention && !item.attentionIconName.empty() ? item.attentionIconName : item.iconName;
  if (!preferred.empty() && GlyphRegistry::contains(preferred)) {
    return preferred;
  }
  if (item.needsAttention) {
    return "warning";
  }
  return "menu-2";
}
