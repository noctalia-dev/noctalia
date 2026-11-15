#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {
  struct Place {
    std::string label;
    std::string identity;
    std::string capacityIdentity;
    std::string details;
    std::filesystem::path path;
    std::vector<std::string> iconNames;
    bool network = false;
    bool mounted = true;
    bool personal = false;
    std::shared_ptr<GVolume> volume;
    std::shared_ptr<GMount> mount;
    bool canMount = false;
    bool canUnmount = false;
    bool canEject = false;
  };

  [[nodiscard]] std::string_view placeGroup(const Place& place);
  // Unknown resources are alphabetic; saved identities keep their relative order.
  void orderPlaces(std::vector<Place>& places, const std::vector<std::string>& order);
  [[nodiscard]] std::vector<std::string> reorderedPlaces(
      const std::vector<Place>& places, std::vector<std::string> order, const std::string& identity,
      const std::string& before
  );

  enum class PlaceOperation { Mount, Unmount, Eject };

  class PlacesModel {
  public:
    using Changed = std::function<void()>;
    PlacesModel();
    ~PlacesModel();
    PlacesModel(const PlacesModel&) = delete;
    PlacesModel& operator=(const PlacesModel&) = delete;
    void setChangedCallback(Changed callback);
    [[nodiscard]] std::vector<Place> discover() const;
    // The operation owns its GIO references until completion. A caller may safely
    // rebuild the sidebar while it runs; completion must guard its own owner.
    static void operate(
        const Place& place, PlaceOperation operation, GMountOperation* prompts, GCancellable* cancellable,
        std::function<void(std::string)> completion
    );
    static void queryCapacity(
        const std::filesystem::path& path, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer userData
    );

  private:
    void scheduleChanged();
    static void monitorChanged(GObject*, gpointer data);
    static void volumeMonitorChanged(GVolumeMonitor*, GObject*, gpointer data);
    static gboolean dispatchChanged(gpointer data);
    GVolumeMonitor* m_volumeMonitor = nullptr;
    GUnixMountMonitor* m_unixMountMonitor = nullptr;
    std::vector<gulong> m_volumeSignals;
    std::vector<gulong> m_unixSignals;
    Changed m_changed;
    guint m_changedSource = 0;
  };
} // namespace filemanager
