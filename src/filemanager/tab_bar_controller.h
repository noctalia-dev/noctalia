#pragma once

#include "filemanager/tab_manager.h"
#include "render/animation/animation_manager.h"
#include "ui/controls/context_menu.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Box;
class Button;
class FileIcon;
class FileManagerTestAccess;
class Flex;
class InputArea;
class InputDispatcher;
class Label;
struct PointerEvent;
class Renderer;
class ScrollView;
class Separator;
class WaylandConnection;

namespace filemanager {

  class TabBarController {
  public:
    struct Callbacks {
      std::function<bool()> isAlwaysShowTabBar;
      std::function<std::filesystem::path()> homePath;
      std::function<void()> onNewTab;
      std::function<void(TabId tabId)> onActivateTab;
      std::function<void(TabId tabId)> onCloseTab;
      std::function<void(TabId tabId, std::size_t targetIndex)> onReorderTab;
      std::function<void(std::vector<ContextMenuControlEntry> entries, float x, float y)> onOpenTabContextMenu;
      std::function<void()> onRequestLayout;
      std::function<void()> onRequestRedraw;
      std::function<bool()> canHandlePointer;
      std::function<std::uint32_t()> pointerSerial;
    };

    TabBarController(
        TabManager& tabs, AnimationManager& animations, InputDispatcher& dispatcher, WaylandConnection& wayland,
        Callbacks callbacks
    );
    ~TabBarController();

    [[nodiscard]] std::unique_ptr<Flex> buildUi();
    void detachUi();

    void markDirty();
    bool rebuildIfDirty();
    void syncLayout(Renderer& renderer);
    void updateTheme();

    bool handlePointer(const PointerEvent& event, float x, float y);
    void cancelGesture();

    [[nodiscard]] bool gestureActive() const noexcept { return m_dragArmed || m_dragging; }
    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] TabId contextMenuTabId() const noexcept { return m_contextMenuTabId; }
    [[nodiscard]] std::vector<ContextMenuControlEntry> tabContextMenuEntries(TabId tabId) const;

  private:
    friend class ::FileManagerTestAccess;

    struct TabSlot {
      TabId tabId = kInvalidTabId;
      Flex* container = nullptr;
      InputArea* hitArea = nullptr;
      FileIcon* icon = nullptr;
      Label* label = nullptr;
      Button* closeButton = nullptr;
      bool active = false;
      bool hovered = false;
      bool administrator = false;
    };

    void updateVisibility();
    void applySlotVisualState(TabSlot& slot);
    void updateDropIndicatorGeometry();
    void positionUnderline();
    void scheduleActiveTabScrollIntoView();
    [[nodiscard]] TabSlot* findSlot(TabId tabId) noexcept;
    [[nodiscard]] const TabSlot* findSlot(TabId tabId) const noexcept;
    [[nodiscard]] std::optional<std::size_t> computeDropInsertionIndex(float sceneX) const;

    TabManager& m_tabs;
    AnimationManager& m_animations;
    InputDispatcher& m_dispatcher;
    WaylandConnection& m_wayland;
    Callbacks m_callbacks;

    Flex* m_container = nullptr;
    Flex* m_stripRow = nullptr;
    ScrollView* m_scroll = nullptr;
    Flex* m_tabsRow = nullptr;
    Button* m_newTabButton = nullptr;
    Separator* m_bottomSeparator = nullptr;
    Box* m_underline = nullptr;
    Box* m_dropIndicator = nullptr;

    std::vector<TabSlot> m_slots;
    bool m_dirty = true;
    bool m_pendingScrollToActive = false;
    float m_lastTabWidth = 0.0F;
    TabId m_lastActiveTabId = kInvalidTabId;

    TabId m_renderedUnderlineTabId = kInvalidTabId;
    AnimationManager::Id m_underlineAnimation = 0;
    float m_underlineProgress = 1.0F;
    float m_underlineStartX = 0.0F;
    float m_underlineStartW = 0.0F;
    float m_underlineTargetX = 0.0F;
    float m_underlineTargetW = 0.0F;
    float m_underlineCurrentX = 0.0F;
    float m_underlineCurrentW = 0.0F;
    bool m_hasUnderlineGeometry = false;

    bool m_dragArmed = false;
    bool m_dragging = false;
    TabId m_dragTabId = kInvalidTabId;
    float m_dragStartX = 0.0F;
    float m_dragStartY = 0.0F;
    std::optional<std::size_t> m_dropInsertIndex;

    TabId m_contextMenuTabId = kInvalidTabId;
  };

} // namespace filemanager
