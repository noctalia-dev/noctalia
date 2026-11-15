#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <string>
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
  };

  class PlacesModel {
  public:
    using Changed = std::function<void()>;
    PlacesModel();
    ~PlacesModel();
    PlacesModel(const PlacesModel&) = delete;
    PlacesModel& operator=(const PlacesModel&) = delete;
    void setChangedCallback(Changed callback);
    [[nodiscard]] std::vector<Place> discover() const;
    static void queryCapacity(
        const std::filesystem::path& path, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer userData
    );

  private:
    void scheduleChanged();
    static void monitorChanged(GObject*, gpointer data);
    static gboolean dispatchChanged(gpointer data);
    GVolumeMonitor* m_volumeMonitor = nullptr;
    GUnixMountMonitor* m_unixMountMonitor = nullptr;
    std::vector<gulong> m_volumeSignals;
    std::vector<gulong> m_unixSignals;
    Changed m_changed;
    guint m_changedSource = 0;
  };
} // namespace filemanager
