#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SessionBus;

struct TrayItemInfo {
  std::string id;
  std::string busName;
  std::string objectPath;
  std::string iconName;
  std::string iconThemePath;
  std::string overlayIconName;
  std::string attentionIconName;
  std::string menuObjectPath;
  std::string itemName;
  std::string processName;
  std::string title;
  std::string statusNotifierTitle;
  std::string statusNotifierDescription;
  std::string status;
  std::vector<std::uint8_t> iconArgb32;
  std::int32_t iconWidth = 0;
  std::int32_t iconHeight = 0;
  std::vector<std::uint8_t> overlayArgb32;
  std::int32_t overlayWidth = 0;
  std::int32_t overlayHeight = 0;
  std::vector<std::uint8_t> attentionArgb32;
  std::int32_t attentionWidth = 0;
  std::int32_t attentionHeight = 0;
  bool needsAttention = false;

  bool operator==(const TrayItemInfo&) const = default;
};

struct TrayMenuEntry {
  std::int32_t id = 0;
  std::string label;
  std::string iconName;
  std::vector<std::uint8_t> iconData;
  bool enabled = true;
  bool visible = true;
  bool separator = false;
  bool hasSubmenu = false;
  bool checkmark = false;
  bool radio = false;
  std::int32_t toggleState = -1;

  bool operator==(const TrayMenuEntry&) const = default;
};

class TrayService {
public:
  using ChangeCallback = std::function<void()>;
  using MenuToggleCallback = std::function<void(const std::string&, float)>;

  explicit TrayService(SessionBus& bus);
  ~TrayService();
  TrayService(const TrayService&) = delete;
  TrayService& operator=(const TrayService&) = delete;

  void start();
  void setChangeCallback(ChangeCallback callback);
  void setMenuToggleCallback(MenuToggleCallback callback);
  void requestMenuToggle(const std::string& itemId, float contentScale = 1.0f) const;
  [[nodiscard]] std::size_t itemCount() const noexcept;
  [[nodiscard]] std::vector<TrayItemInfo> items() const;
  [[nodiscard]] bool itemUsesDBusMenu(const std::string& itemId) const;
  [[nodiscard]] std::vector<TrayMenuEntry> menuEntries(const std::string& itemId);
  [[nodiscard]] std::vector<TrayMenuEntry> menuEntriesForParent(const std::string& itemId, std::int32_t parentId);
  // Returns true if the click event was dispatched to DBus successfully.
  // This does not imply the remote menu action completed successfully.
  [[nodiscard]] bool activateMenuEntry(const std::string& itemId, std::int32_t entryId);
  // Notify the dbusmenu server that a (sub)menu is being opened/closed. `entryId`
  // is the menu item id: 0 for the root menu, or a submenu parent id otherwise.
  // Well-behaved dbusmenu servers (including Electron) rely on paired opened/closed
  // events to reset internal state — skipping these causes state drift after many
  // open/close cycles.
  void notifyMenuOpened(const std::string& itemId, std::int32_t entryId = 0);
  void notifyMenuClosed(const std::string& itemId, std::int32_t entryId = 0);
  [[nodiscard]] std::vector<std::string> registeredItems() const;
  [[nodiscard]] bool activateItem(const std::string& itemId, std::int32_t x = 0, std::int32_t y = 0);
  [[nodiscard]] bool openContextMenu(const std::string& itemId, std::int32_t x = 0, std::int32_t y = 0);

private:
  struct MenuCache {
    std::unique_ptr<sdbus::IProxy> proxy;
    std::unordered_map<std::int32_t, TrayMenuEntry> entriesById;
    // Decoded child ids per parent-id. parentId=0 is the root menu.
    std::unordered_map<std::int32_t, std::vector<std::int32_t>> childrenByParent;
    std::unordered_map<std::int32_t, std::chrono::steady_clock::time_point> nextRetryAt;
    std::unordered_map<std::int32_t, std::uint8_t> failureStreak;
    std::unordered_map<std::int32_t, std::uint32_t> lastLayoutUpdatedRevisionByParent;
    std::unordered_set<std::int32_t> loadedParents;
    std::unordered_set<std::int32_t> loadingParents;
    std::uint32_t revision = 0;
    std::uint64_t generation = 0;
    bool rootLoaded = false;
    bool rootAboutToShowPrimed = false;
  };

  void onRegisterStatusNotifierItem(const std::string& serviceOrPath, const std::string& senderBusName);
  void onRegisterStatusNotifierHost(const std::string& host);
  void startLegacyOwner();
  void startKde();
  void startAsWatcherOwner();
  void startAsWatcherClient();
  void connectToExternalWatcher();
  void disconnectExternalWatcher();
  void onExternalItemRegistered(const std::string& service);
  void onExternalItemUnregistered(const std::string& service);
  [[nodiscard]] bool externalWatcherHasOwner() const;
  void notifyWatcherItemRegistered(const std::string& itemId);
  void notifyWatcherItemUnregistered(const std::string& itemId);
  void removeItemById(const std::string& itemId);
  void discoverExistingItems();
  void tryRegisterItemForBusName(const std::string& busName, std::function<void(bool)> callback = {});
  void scheduleBusOnlyRegistrationProbe(const std::string& busName, int retriesRemaining);
  void scheduleMetadataRefreshRetry(const std::string& itemId, int retriesRemaining);
  [[nodiscard]] bool isMetadataReady(const TrayItemInfo& item) const;
  void registerOrRefreshItem(const std::string& busName, const std::string& objectPath);
  void attachItemProxySignals(const std::string& itemId, sdbus::IProxy& proxy);
  void resolvePathOnlyItemProxy(const std::string& itemId);
  void requestProcessNameForItem(const std::string& itemId, const std::string& busName);
  [[nodiscard]] std::uint32_t connectionPidForBusName(const std::string& busName) const;
  void refreshItemMetadata(const std::string& itemId);
  void ensureMenuCache(const std::string& itemId, const std::string& busName, const std::string& menuPath);
  void dropMenuCache(const std::string& itemId);
  void fetchMenuProperties(
      const std::string& itemId, const std::vector<std::int32_t>& entryIds, std::function<void(bool)> callback
  );
  void requestMenuSubtree(const std::string& itemId, std::int32_t parentId, bool force = false);
  void requestMenuLayoutAfterAboutToShow(const std::string& itemId, std::int32_t parentId, std::uint64_t generation);
  void sendMenuEvent(const std::string& itemId, std::int32_t entryId, const std::string& eventName);
  [[nodiscard]] bool ensureItemProxy(const std::string& itemId);
  void removeItemsForBusName(const std::string& busName);
  void emitChanged();

  [[nodiscard]] static std::string busNameFromItemId(const std::string& itemId);
  [[nodiscard]] static std::string canonicalItemId(const std::string& busName, const std::string& objectPath);

  SessionBus& m_bus;
  enum class WatcherRole { Owner, Client };
  WatcherRole m_watcherRole = WatcherRole::Owner;
  std::string m_hostBusName;
  std::unique_ptr<sdbus::IObject> m_watcherObject;
  std::unique_ptr<sdbus::IProxy> m_watcherProxy;
  std::unique_ptr<sdbus::IProxy> m_dbusProxy;
  std::unordered_map<std::string, TrayItemInfo> m_items;
  std::unordered_map<std::string, std::unique_ptr<sdbus::IProxy>> m_itemProxies;
  std::unordered_map<std::string, MenuCache> m_menuCache;
  std::unordered_set<std::string> m_pathOnlyResolutionsInFlight;
  bool m_hostRegistered = true;
  bool m_externalWatcherConnected = false;
  bool m_started = false;
  ChangeCallback m_changeCallback;
  MenuToggleCallback m_menuToggleCallback;
};
