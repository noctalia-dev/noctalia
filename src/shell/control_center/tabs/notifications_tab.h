#pragma once

#include "render/animation/animation_manager.h"
#include "shell/control_center/tab.h"
#include "system/icon_resolver.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class NotificationManager;
class CompositorPlatform;
class ConfigService;
struct NotificationHistoryEntry;
class Button;
class Segmented;
class VirtualListView;
class Label;
class NotificationHistoryAdapter;

enum class NotificationDisplayItemKind : uint8_t {
  Single,         // single notification
  CollapsedGroup, // group bundle, shows latest + count badge
  GroupHeader,    // header bar for an expanded group
  GroupChild      // individual message under the expanded group
};

struct NotificationDisplayItem {
  NotificationDisplayItemKind kind = NotificationDisplayItemKind::Single;
  uint32_t notificationId = 0;
  std::string groupKey;
  std::string appName;
  std::size_t total_count = 1;
  std::size_t childIndex = 0;
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
  void removeNotificationEntry(uint32_t id, bool wasActive);
  void removeGroupEntries(const std::string& key);
  void toggleDoNotDisturb();
  void toggleNotificationExpanded(uint32_t id);
  void toggleGroupExpanded(const std::string& key);
  void invokeNotificationAction(uint32_t id, const std::string& actionKey);
  bool refreshDataSnapshot();
  void syncDndButton();
  void updateEmptyState(bool hasHistory, bool hasFiltered);
  std::optional<std::size_t> filteredIndexForId(uint32_t id) const;
  // The filter slide keeps its snapshot; this only removes ids history no longer holds.
  void dropVanishedFiltered();
  void cancelFilterSlide();
  void beginFilterSlideOut(std::size_t nextIndex);
  void beginFilterSlideIn();
  void applyFilterSlide(float progress, bool slidingIn);
  [[nodiscard]] bool filterSlideOutActive() const;

  NotificationManager* m_notifications = nullptr;
  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  bool m_lastGroupByApp = true;
  IconResolver m_iconResolver;
  std::unique_ptr<NotificationHistoryAdapter> m_adapter;
  std::vector<NotificationDisplayItem> m_filtered;
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
  std::unordered_set<std::string> m_expandedGroupKeys;
  std::uint64_t m_lastSerial = 0;
  /// Wall-clock coarse slot so relative times (e.g. "2 min ago") refresh without churning every frame.
  std::int64_t m_lastRelativeTimeSlot = -1;
  std::size_t m_lastRebuildFilterIndex = static_cast<std::size_t>(-1);
  std::size_t m_pendingFilterIndex = std::numeric_limits<std::size_t>::max();
  bool m_startFilterSlideIn = false;
  int m_filterSlideDirection = 0;
  float m_filterSlideBaseX = 0.0F;
  float m_filterSlideBaseY = 0.0F;
  AnimationManager::Id m_filterSlideAnimId = 0;
};
