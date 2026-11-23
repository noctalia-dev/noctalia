#include "filemanager/tab_bar_controller.h"

#include "cursor-shape-v1-client-protocol.h"
#include "filemanager/file_menu_controller.h"
#include "filemanager/tab_presentation.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/dialogs/file_icon.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>

namespace filemanager {

  namespace {

    constexpr float kTabBarHeight = 32.0F;
    constexpr float kTabGap = 2.0F;
    constexpr float kStripGap = 4.0F;
    constexpr float kMinTabWidth = 112.0F;
    constexpr float kMaxTabWidth = 220.0F;
    constexpr float kCloseButtonSize = 20.0F;
    constexpr float kNewTabButtonSize = 26.0F;
    constexpr float kIconSize = 16.0F;
    constexpr float kUnderlineHeight = 2.0F;
    constexpr float kDropIndicatorWidth = 2.0F;
    constexpr float kDropIndicatorHeight = 24.0F;
    constexpr float kUnderlineDurationMs = 180.0F;
    constexpr float kDragThresholdPx = 5.0F;

    constexpr float kActiveTabTintAlpha = 0.14F;
    constexpr float kHoverTabTintAlpha = 0.07F;
    constexpr float kTabBarSeparatorAlpha = 0.45F;

  } // namespace

  TabBarController::TabBarController(
      TabManager& tabs, AnimationManager& animations, InputDispatcher& dispatcher, WaylandConnection& wayland,
      Callbacks callbacks
  )
      : m_tabs(tabs),
        m_animations(animations),
        m_dispatcher(dispatcher),
        m_wayland(wayland),
        m_callbacks(std::move(callbacks)) {}

  TabBarController::~TabBarController() { detachUi(); }

  std::unique_ptr<Flex> TabBarController::buildUi() {
    detachUi();

    auto container = std::make_unique<Flex>();
    container->setDirection(FlexDirection::Vertical);
    container->setAlign(FlexAlign::Stretch);
    container->setGap(0.0F);
    m_container = container.get();

    auto stripRow = std::make_unique<Flex>();
    stripRow->setDirection(FlexDirection::Horizontal);
    stripRow->setAlign(FlexAlign::Center);
    stripRow->setGap(kStripGap);
    stripRow->setMinHeight(kTabBarHeight);
    stripRow->setMaxHeight(kTabBarHeight);
    m_stripRow = stripRow.get();

    auto scroll = std::make_unique<ScrollView>();
    scroll->setOrientation(ScrollOrientation::Horizontal);
    scroll->setFlexGrow(1.0F);
    scroll->setMinWidth(0.0F);
    scroll->setFillHeight(true);
    scroll->setMinHeight(kTabBarHeight);
    scroll->setMaxHeight(kTabBarHeight);
    scroll->setScrollbarVisible(false);
    scroll->setReserveScrollbarGutter(false);
    scroll->setViewportPaddingH(0.0F);
    scroll->setViewportPaddingV(0.0F);
    scroll->setMapVerticalWheelToHorizontal(true);
    scroll->setClipChildren(true);
    m_scroll = scroll.get();

    m_tabsRow = m_scroll->content();
    m_tabsRow->setDirection(FlexDirection::Horizontal);
    m_tabsRow->setAlign(FlexAlign::Stretch);
    m_tabsRow->setGap(kTabGap);
    m_tabsRow->setMinHeight(kTabBarHeight);
    m_tabsRow->setMaxHeight(kTabBarHeight);

    auto underline = std::make_unique<Box>();
    underline->setFill(colorSpecFromRole(ColorRole::Primary));
    underline->setRadius(1.0F);
    underline->setHitTestVisible(false);
    underline->setParticipatesInLayout(false);
    underline->setZIndex(3);
    m_underline = static_cast<Box*>(m_tabsRow->addChild(std::move(underline)));

    auto dropIndicator = std::make_unique<Box>();
    dropIndicator->setFill(colorSpecFromRole(ColorRole::Primary));
    dropIndicator->setRadius(1.0F);
    dropIndicator->setVisible(false);
    dropIndicator->setHitTestVisible(false);
    dropIndicator->setParticipatesInLayout(false);
    dropIndicator->setZIndex(4);
    m_dropIndicator = static_cast<Box*>(m_tabsRow->addChild(std::move(dropIndicator)));

    stripRow->addChild(std::move(scroll));

    auto newTabButton = std::make_unique<Button>();
    newTabButton->setVariant(ButtonVariant::Ghost);
    newTabButton->setText("+");
    newTabButton->setControlHeight(kNewTabButtonSize);
    newTabButton->setMinWidth(kNewTabButtonSize);
    newTabButton->setMaxWidth(kNewTabButtonSize);
    newTabButton->setPadding(0.0F);
    newTabButton->setFocusHighlightEnabled(false);
    newTabButton->setTooltip("New Tab (Ctrl+T)");
    newTabButton->setOnClick([this] {
      if (m_callbacks.onNewTab)
        m_callbacks.onNewTab();
    });
    m_newTabButton = static_cast<Button*>(stripRow->addChild(std::move(newTabButton)));

    container->addChild(std::move(stripRow));
    container->addChild(
        ui::separator({
            .out = &m_bottomSeparator,
            .color = colorSpecFromRole(ColorRole::OutlineVariant, kTabBarSeparatorAlpha),
            .gradientEdges = false,
        })
    );

    m_dirty = true;
    updateVisibility();
    return container;
  }

  void TabBarController::detachUi() {
    m_animations.cancel(m_underlineAnimation);
    m_underlineAnimation = 0;
    cancelGesture();
    m_slots.clear();
    m_container = nullptr;
    m_stripRow = nullptr;
    m_scroll = nullptr;
    m_tabsRow = nullptr;
    m_newTabButton = nullptr;
    m_bottomSeparator = nullptr;
    m_underline = nullptr;
    m_dropIndicator = nullptr;
    m_hasUnderlineGeometry = false;
    m_renderedUnderlineTabId = kInvalidTabId;
    m_lastActiveTabId = kInvalidTabId;
    m_lastTabWidth = 0.0F;
    m_dirty = true;
  }

  void TabBarController::markDirty() {
    m_dirty = true;
    updateVisibility();
  }

  bool TabBarController::visible() const noexcept { return m_container != nullptr && m_container->visible(); }

  void TabBarController::updateVisibility() {
    if (m_container == nullptr)
      return;
    const bool alwaysShow = m_callbacks.isAlwaysShowTabBar && m_callbacks.isAlwaysShowTabBar();
    const bool show = m_tabs.count() > 1 || alwaysShow;
    m_container->setVisible(show);
    m_container->setParticipatesInLayout(show);
  }

  bool TabBarController::rebuildIfDirty() {
    if (!m_dirty || m_tabsRow == nullptr)
      return false;
    m_dirty = false;
    updateVisibility();

    for (const auto& slot : m_slots) {
      if (slot.container != nullptr)
        (void)m_tabsRow->removeChild(slot.container);
    }
    m_slots.clear();

    const std::filesystem::path home = m_callbacks.homePath ? m_callbacks.homePath() : std::filesystem::path{};
    const TabId activeId = m_tabs.activeTabId();
    if (activeId != m_lastActiveTabId) {
      m_pendingScrollToActive = true;
      m_lastActiveTabId = activeId;
    }

    const float initialWidth = m_lastTabWidth >= kMinTabWidth && m_lastTabWidth <= kMaxTabWidth ? m_lastTabWidth
                                                                                                : kMaxTabWidth;
    for (const auto& tabPtr : m_tabs.tabs()) {
      const TabId tabId = tabPtr->id;
      const auto path = tabPtr->session.path();
      const bool active = tabId == activeId;
      const bool administrator = tabPtr->hasActiveAdminSession();

      auto slotNode = std::make_unique<Flex>();
      slotNode->setDirection(FlexDirection::Horizontal);
      slotNode->setAlign(FlexAlign::Center);
      slotNode->setGap(6.0F);
      slotNode->setPadding(0.0F, 8.0F, 0.0F, 8.0F);
      slotNode->setMinHeight(kTabBarHeight);
      slotNode->setMaxHeight(kTabBarHeight);
      slotNode->setMinWidth(initialWidth);
      slotNode->setMaxWidth(initialWidth);
      slotNode->setRadius(Style::scaledRadiusSm(1.0F));
      slotNode->setClipChildren(true);

      auto icon = std::make_unique<FileIcon>();
      icon->setGlyphSize(kIconSize);
      icon->setFallbackGlyph(isTrashTabPath(path) ? "trash" : "folder");
      icon->setHitTestVisible(false);
      icon->setAllowFilesystemQueries(!administrator);
      const auto iconNames = tabIconNamesForPath(path, home);
      if (!iconNames.empty())
        icon->setThemedIconNames(iconNames);
      else
        icon->setFile(path, true);
      auto* iconPtr = static_cast<FileIcon*>(slotNode->addChild(std::move(icon)));

      auto label = std::make_unique<Label>();
      label->setText(tabDisplayLabel(path, home));
      label->setFlexGrow(1.0F);
      label->setMinWidth(0.0F);
      label->setMaxLines(1);
      label->setEllipsize(TextEllipsize::End);
      label->setFontWeight(FontWeight::Medium);
      label->setHitTestVisible(false);
      auto* labelPtr = static_cast<Label*>(slotNode->addChild(std::move(label)));

      auto hitArea = std::make_unique<InputArea>();
      hitArea->setParticipatesInLayout(false);
      hitArea->setZIndex(1);
      hitArea->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
      hitArea->setAcceptedScrollDirections(0);
      hitArea->setTooltip(tabTooltipForPath(path));
      hitArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
      auto* hitAreaPtr = static_cast<InputArea*>(slotNode->addChild(std::move(hitArea)));

      auto closeBtn = std::make_unique<Button>();
      closeBtn->setVariant(ButtonVariant::Ghost);
      closeBtn->setText("\xC3\x97");
      closeBtn->setControlHeight(kCloseButtonSize);
      closeBtn->setMinWidth(kCloseButtonSize);
      closeBtn->setMaxWidth(kCloseButtonSize);
      closeBtn->setPadding(0.0F);
      closeBtn->setTabStop(false);
      closeBtn->setFocusHighlightEnabled(false);
      closeBtn->setZIndex(2);
      closeBtn->setTooltip("Close Tab");
      closeBtn->setOnClick([this, tabId] {
        if (m_tabs.findTab(tabId) != nullptr && m_callbacks.onCloseTab)
          m_callbacks.onCloseTab(tabId);
      });
      auto* closeBtnPtr = static_cast<Button*>(slotNode->addChild(std::move(closeBtn)));

      auto* slotPtr = static_cast<Flex*>(m_tabsRow->addChild(std::move(slotNode)));
      TabSlot slot{
          .tabId = tabId,
          .container = slotPtr,
          .hitArea = hitAreaPtr,
          .icon = iconPtr,
          .label = labelPtr,
          .closeButton = closeBtnPtr,
          .active = active,
          .hovered = false,
          .administrator = administrator,
      };
      applySlotVisualState(slot);
      m_slots.push_back(slot);

      hitAreaPtr->setOnEnter([this, tabId](const InputArea::PointerData&) {
        if (auto* found = findSlot(tabId)) {
          found->hovered = true;
          applySlotVisualState(*found);
          if (m_callbacks.onRequestRedraw)
            m_callbacks.onRequestRedraw();
        }
      });
      hitAreaPtr->setOnLeave([this, tabId] {
        if (auto* found = findSlot(tabId)) {
          found->hovered = false;
          applySlotVisualState(*found);
          if (m_callbacks.onRequestRedraw)
            m_callbacks.onRequestRedraw();
        }
      });
      hitAreaPtr->setOnPress([this, tabId](const InputArea::PointerData& data) {
        if (m_tabs.findTab(tabId) == nullptr)
          return;
        if (data.button == BTN_LEFT && data.pressed) {
          if (m_tabs.activeTabId() != tabId && m_callbacks.onActivateTab)
            m_callbacks.onActivateTab(tabId);
          if (m_tabs.count() > 1 && (!m_callbacks.canHandlePointer || m_callbacks.canHandlePointer())) {
            m_dragArmed = true;
            m_dragging = false;
            m_dragTabId = tabId;
            m_dragStartX = data.sceneX;
            m_dragStartY = data.sceneY;
            m_dropInsertIndex.reset();
          }
          return;
        }
        if (data.button == BTN_RIGHT && data.pressed) {
          m_contextMenuTabId = tabId;
          if (m_callbacks.onOpenTabContextMenu)
            m_callbacks.onOpenTabContextMenu(tabContextMenuEntries(tabId), data.sceneX, data.sceneY);
        }
      });
      hitAreaPtr->setOnClick([this, tabId](const InputArea::PointerData& data) {
        if (data.button == BTN_MIDDLE && m_tabs.findTab(tabId) != nullptr && m_callbacks.onCloseTab)
          m_callbacks.onCloseTab(tabId);
      });
    }
    return true;
  }

  void TabBarController::applySlotVisualState(TabSlot& slot) {
    if (slot.container == nullptr)
      return;
    if (const auto* tab = m_tabs.findTab(slot.tabId))
      slot.administrator = tab->hasActiveAdminSession();
    if (slot.active)
      slot.container->setFill(colorSpecFromRole(ColorRole::Primary, kActiveTabTintAlpha));
    else if (slot.hovered)
      slot.container->setFill(colorSpecFromRole(ColorRole::OnSurface, kHoverTabTintAlpha));
    else
      slot.container->setFill(clearColorSpec());
    if (slot.label != nullptr) {
      const ColorRole labelRole = slot.administrator ? kAdminWarningColorRole
          : slot.active                              ? ColorRole::OnSurface
                                                     : ColorRole::OnSurfaceVariant;
      slot.label->setColor(colorSpecFromRole(labelRole));
    }
    if (slot.icon != nullptr)
      slot.icon->setColor(colorForRole(slot.active ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
  }

  void TabBarController::syncLayout(Renderer& renderer) {
    if (m_container == nullptr || !m_container->visible() || m_scroll == nullptr || m_tabsRow == nullptr)
      return;

    const float viewportW = m_scroll->contentViewportWidth();
    bool needTabBarRelayout = false;
    if (viewportW > 0.0F && !m_slots.empty()) {
      const float count = static_cast<float>(m_slots.size());
      const float available = std::max(0.0F, viewportW - kTabGap * std::max(0.0F, count - 1.0F));
      const float targetWidth = std::clamp(available / count, kMinTabWidth, kMaxTabWidth);
      if (std::abs(targetWidth - m_lastTabWidth) > 0.25F) {
        m_lastTabWidth = targetWidth;
        needTabBarRelayout = true;
      }
      for (const auto& slot : m_slots) {
        if (slot.container != nullptr && std::abs(slot.container->width() - targetWidth) > 0.25F) {
          slot.container->setMinWidth(targetWidth);
          slot.container->setMaxWidth(targetWidth);
          needTabBarRelayout = true;
        }
      }
    }

    if (m_pendingScrollToActive) {
      m_pendingScrollToActive = false;
      if (const auto* active = findSlot(m_tabs.activeTabId()); active != nullptr && active->container != nullptr) {
        m_scroll->requestScrollIntoView(active->container);
        needTabBarRelayout = true;
      }
    }

    if (needTabBarRelayout) {
      m_scroll->layout(renderer);
    }

    for (const auto& slot : m_slots) {
      if (slot.container != nullptr && slot.hitArea != nullptr) {
        slot.hitArea->setPosition(0.0F, 0.0F);
        slot.hitArea->setSize(slot.container->width(), slot.container->height());
      }
    }

    positionUnderline();
    updateDropIndicatorGeometry();
  }

  void TabBarController::positionUnderline() {
    if (m_underline == nullptr || m_tabsRow == nullptr)
      return;
    const auto* active = findSlot(m_tabs.activeTabId());
    if (active == nullptr || active->container == nullptr) {
      m_underline->setVisible(false);
      return;
    }
    m_underline->setVisible(true);
    const float targetX = active->container->x();
    const float targetW = active->container->width();
    const float targetY = std::max(0.0F, active->container->y() + active->container->height() - kUnderlineHeight);

    const bool switched = m_hasUnderlineGeometry
        && m_renderedUnderlineTabId != kInvalidTabId
        && m_renderedUnderlineTabId != active->tabId;
    m_renderedUnderlineTabId = active->tabId;

    if (switched) {
      m_animations.cancel(m_underlineAnimation);
      m_underlineStartX = m_underlineCurrentX;
      m_underlineStartW = m_underlineCurrentW;
      m_underlineTargetX = targetX;
      m_underlineTargetW = targetW;
      m_underlineProgress = 0.0F;
      m_underlineAnimation = m_animations.animate(
          0.0F, 1.0F, kUnderlineDurationMs, Easing::EaseOutCubic,
          [this, targetY](float progress) {
            m_underlineProgress = progress;
            m_underlineCurrentX = std::lerp(m_underlineStartX, m_underlineTargetX, m_underlineProgress);
            m_underlineCurrentW = std::lerp(m_underlineStartW, m_underlineTargetW, m_underlineProgress);
            if (m_underline != nullptr) {
              m_underline->setPosition(m_underlineCurrentX, targetY);
              m_underline->setFrameSize(m_underlineCurrentW, kUnderlineHeight);
            }
            if (m_callbacks.onRequestRedraw)
              m_callbacks.onRequestRedraw();
          },
          [this] { m_underlineAnimation = 0; },
          this
      );
      return;
    }

    if (m_underlineAnimation != 0) {
      m_underlineTargetX = targetX;
      m_underlineTargetW = targetW;
      m_underlineCurrentX = std::lerp(m_underlineStartX, m_underlineTargetX, m_underlineProgress);
      m_underlineCurrentW = std::lerp(m_underlineStartW, m_underlineTargetW, m_underlineProgress);
      m_underline->setPosition(m_underlineCurrentX, targetY);
      m_underline->setFrameSize(m_underlineCurrentW, kUnderlineHeight);
      return;
    }

    m_underlineCurrentX = targetX;
    m_underlineCurrentW = targetW;
    m_underlineTargetX = targetX;
    m_underlineTargetW = targetW;
    m_hasUnderlineGeometry = true;
    m_underline->setPosition(targetX, targetY);
    m_underline->setFrameSize(targetW, kUnderlineHeight);
  }

  void TabBarController::updateTheme() {
    for (auto& slot : m_slots)
      applySlotVisualState(slot);
    if (m_underline != nullptr)
      m_underline->setFill(colorSpecFromRole(ColorRole::Primary));
    if (m_dropIndicator != nullptr)
      m_dropIndicator->setFill(colorSpecFromRole(ColorRole::Primary));
    if (m_bottomSeparator != nullptr)
      m_bottomSeparator->setColor(colorSpecFromRole(ColorRole::OutlineVariant, kTabBarSeparatorAlpha));
  }

  std::optional<std::size_t> TabBarController::computeDropInsertionIndex(float sceneX) const {
    if (m_tabsRow == nullptr || m_slots.empty())
      return std::nullopt;
    float rowX = 0.0F;
    float rowY = 0.0F;
    Node::absolutePosition(m_tabsRow, rowX, rowY);
    const float localX = sceneX - rowX;
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
      const auto* slotNode = m_slots[i].container;
      if (slotNode == nullptr)
        continue;
      const float midpoint = slotNode->x() + slotNode->width() * 0.5F;
      if (localX < midpoint)
        return i;
    }
    return m_slots.size();
  }

  void TabBarController::updateDropIndicatorGeometry() {
    if (m_dropIndicator == nullptr || m_tabsRow == nullptr)
      return;
    if (!m_dragging || !m_dropInsertIndex || m_slots.empty()) {
      m_dropIndicator->setVisible(false);
      return;
    }
    const std::size_t idx = std::min(*m_dropInsertIndex, m_slots.size());
    float indicatorX = 0.0F;
    if (idx == 0)
      indicatorX = m_slots.front().container->x();
    else if (idx >= m_slots.size())
      indicatorX = m_slots.back().container->x() + m_slots.back().container->width() - kDropIndicatorWidth;
    else {
      const float prevEnd = m_slots[idx - 1].container->x() + m_slots[idx - 1].container->width();
      const float nextStart = m_slots[idx].container->x();
      indicatorX = (prevEnd + nextStart - kDropIndicatorWidth) * 0.5F;
    }
    const float indicatorY = std::max(0.0F, (kTabBarHeight - kDropIndicatorHeight) * 0.5F);
    m_dropIndicator->setPosition(indicatorX, indicatorY);
    m_dropIndicator->setFrameSize(kDropIndicatorWidth, kDropIndicatorHeight);
    m_dropIndicator->setVisible(true);
  }

  bool TabBarController::handlePointer(const PointerEvent& event, float x, float y) {
    if (!m_dragArmed && !m_dragging)
      return false;

    if (event.type == PointerEvent::Type::Motion) {
      if (!m_dragging && std::hypot(x - m_dragStartX, y - m_dragStartY) > kDragThresholdPx) {
        m_dragging = true;
        m_dispatcher.cancelPointerCapture();
        TooltipManager::instance().forceDestroy();
        if (m_callbacks.pointerSerial) {
          if (const std::uint32_t serial = m_callbacks.pointerSerial(); serial != 0)
            m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING);
        }
      }
      if (m_dragging) {
        m_dropInsertIndex = computeDropInsertionIndex(x);
        updateDropIndicatorGeometry();
        if (m_callbacks.onRequestRedraw)
          m_callbacks.onRequestRedraw();
        return true;
      }
      return false;
    }

    if (event.type == PointerEvent::Type::Button && event.button == BTN_LEFT && !event.pressed) {
      if (m_dragging) {
        if (m_dropInsertIndex) {
          if (const auto srcIdx = m_tabs.indexOf(m_dragTabId)) {
            const std::size_t insertBefore = *m_dropInsertIndex;
            if (insertBefore != *srcIdx && insertBefore != *srcIdx + 1) {
              const std::size_t targetIdx = insertBefore > *srcIdx ? insertBefore - 1 : insertBefore;
              if (m_callbacks.onReorderTab)
                m_callbacks.onReorderTab(m_dragTabId, targetIdx);
            }
          }
        }
        cancelGesture();
        return true;
      }
      m_dragArmed = false;
      m_dragTabId = kInvalidTabId;
      return false;
    }

    return m_dragging;
  }

  void TabBarController::cancelGesture() {
    const bool wasDragging = m_dragging;
    m_dragArmed = false;
    m_dragging = false;
    m_dragTabId = kInvalidTabId;
    m_dropInsertIndex.reset();
    if (m_dropIndicator != nullptr)
      m_dropIndicator->setVisible(false);
    if (wasDragging && m_callbacks.pointerSerial) {
      if (const std::uint32_t serial = m_callbacks.pointerSerial(); serial != 0)
        m_wayland.setCursorShape(serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    }
    if (wasDragging && m_callbacks.onRequestRedraw)
      m_callbacks.onRequestRedraw();
  }

  std::vector<ContextMenuControlEntry> TabBarController::tabContextMenuEntries(TabId tabId) const {
    const std::size_t count = m_tabs.count();
    const auto idx = m_tabs.indexOf(tabId);
    const bool canCloseOthers = count > 1 && idx.has_value();
    const bool canCloseRight = idx.has_value() && (*idx + 1 < count);
    return {
        {.id = menu_id::kTabContextNew,
         .label = "New Tab",
         .shortcut = "Ctrl+T",
         .actionIconNames = {"tab-new-symbolic", "tab-new"},
         .actionGlyph = "plus"},
        {.id = menu_id::kTabContextDuplicate,
         .label = "Duplicate Tab",
         .enabled = idx.has_value(),
         .actionGlyph = "copy"},
        {.separator = true},
        {.id = menu_id::kTabContextClose,
         .label = "Close Tab",
         .enabled = idx.has_value(),
         .shortcut = "Ctrl+W",
         .actionGlyph = "x"},
        {.id = menu_id::kTabContextCloseOthers, .label = "Close Other Tabs", .enabled = canCloseOthers},
        {.id = menu_id::kTabContextCloseRight, .label = "Close Tabs to the Right", .enabled = canCloseRight},
    };
  }

  TabBarController::TabSlot* TabBarController::findSlot(TabId tabId) noexcept {
    const auto it = std::ranges::find(m_slots, tabId, &TabSlot::tabId);
    return it == m_slots.end() ? nullptr : &*it;
  }

  const TabBarController::TabSlot* TabBarController::findSlot(TabId tabId) const noexcept {
    const auto it = std::ranges::find(m_slots, tabId, &TabSlot::tabId);
    return it == m_slots.end() ? nullptr : &*it;
  }

} // namespace filemanager
