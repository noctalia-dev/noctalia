#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/tab.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class NotificationManager;
class CompositorPlatform;
class ConfigService;
struct Notification;
struct NotificationHistoryEntry;
class Button;
class Segmented;
class VirtualListView;
class Label;
class NotificationHistoryAdapter;
struct AppNotificationGroup;
struct HistoryRowItem;

namespace notification_grouping {

  [[nodiscard]] std::string normalizeAppKey(std::string_view key);
  [[nodiscard]] std::string groupKey(const Notification& notification);
  [[nodiscard]] std::string groupDisplayName(const Notification& notification);
  bool pruneGroups(
      std::vector<AppNotificationGroup>& groups, const std::function<bool(uint32_t)>& isVanished,
      std::unordered_set<std::string>* expandedAppKeys = nullptr
  );
  [[nodiscard]] std::vector<HistoryRowItem> buildVisualRows(
      const std::vector<AppNotificationGroup>& groups, std::unordered_set<std::string>& expandedAppKeys,
      bool groupByApp = true
  );
  [[nodiscard]] bool isRowStackToggleInteractive(const HistoryRowItem& row) noexcept;

} // namespace notification_grouping

struct AppNotificationGroup {
  std::string appKey;
  std::string appDisplayName;
  uint32_t representativeId = 0;
  std::vector<uint32_t> notificationIds;
};

struct HistoryRowItem {
  enum class Kind : uint8_t {
    AppHeader,
    StackedCard,
    Notification,
  };

  Kind kind = Kind::Notification;
  std::string appKey;
  std::string appDisplayName;
  uint32_t notificationId = 0;
  uint32_t representativeId = 0;
  std::size_t notificationCount = 0;
  bool collapsed = false;
  bool isGroupParent = false;
  bool hasAppHeader = false;
};

class NotificationsTab : public Tab {
public:
  NotificationsTab(NotificationManager* notifications, CompositorPlatform* platform, ConfigService* config = nullptr);
  ~NotificationsTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  bool handleKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  friend class NotificationHistoryAdapter;

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void clearAllNotifications();
  void clearAppGroup(const std::string& appKey);
  void toggleAppGroupExpanded(const std::string& appKey);
  void removeNotificationEntry(uint32_t id, bool wasActive);
  void toggleDoNotDisturb();
  void toggleNotificationExpanded(uint32_t id);
  void invokeNotificationAction(uint32_t id, const std::string& actionKey);
  bool refreshDataSnapshot();
  void rebuildVisualRows();
  void syncDndButton();
  void updateEmptyState(bool hasHistory, bool hasFiltered);
  std::optional<std::size_t> filteredIndexForId(uint32_t id) const;
  [[nodiscard]] const NotificationHistoryEntry* filteredEntry(std::size_t index) const;
  void syncSnapshotAfterMutation();
  // The filter slide keeps its snapshot; this only removes ids history no longer holds.
  void dropVanishedFiltered();
  void cancelFilterSlide();
  void beginFilterSlideOut(std::size_t nextIndex);
  void beginFilterSlideIn();
  void applyFilterSlide(float progress, bool slidingIn);
  [[nodiscard]] bool filterSlideOutActive() const;

  [[nodiscard]] bool groupHistoryByApp() const noexcept;

  NotificationManager* m_notifications = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  IconResolver m_iconResolver;
  std::unique_ptr<NotificationHistoryAdapter> m_adapter;
  // Notification groups from the current filter snapshot, and the visual rows derived from them.
  std::vector<AppNotificationGroup> m_appGroups;
  std::vector<HistoryRowItem> m_rows;
  std::unordered_set<std::string> m_expandedAppKeys;
  Flex* m_root = nullptr;
  VirtualListView* m_list = nullptr;
  Flex* m_emptyCard = nullptr;
  Label* m_emptyTitle = nullptr;
  Label* m_emptyBody = nullptr;
  Button* m_clearAllButton = nullptr;
  Button* m_dndButton = nullptr;
  Segmented* m_filter = nullptr;
  std::size_t m_filterIndex = 0;
  std::unordered_set<uint32_t> m_expandedIds;
  std::uint64_t m_lastSerial = 0;
  /// Wall-clock coarse slot so relative times (e.g. "2 min ago") refresh without churning every frame.
  std::int64_t m_lastRelativeTimeSlot = -1;
  std::size_t m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
  bool m_lastGroupByApp = true;
  std::size_t m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  bool m_startFilterSlideIn = false;
  int m_filterSlideDirection = 0;
  float m_filterSlideBaseX = 0.0F;
  float m_filterSlideBaseY = 0.0F;
  AnimationManager::Id m_filterSlideAnimId = 0;
};
