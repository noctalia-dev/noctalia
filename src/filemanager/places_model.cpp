#include "filemanager/places_model.h"

#include "filemanager/place_metadata.h"

#include <algorithm>
#include <gio/gunixmounts.h>
#include <ranges>
#include <set>
#include <string_view>
#include <unistd.h>

namespace filemanager {
  namespace {
    bool networkFs(std::string_view type, std::string_view /*source*/) {
      // A local FUSE mount can expose remote-backed storage without being a
      // protocol-mounted share. Classify from the filesystem protocol, not
      // a source spelling such as rclone's "remote:/folder".
      return type == "nfs"
          || type == "nfs4"
          || type == "cifs"
          || type == "smb3"
          || type == "smbfs"
          || type == "sshfs"
          || type == "fuse.sshfs";
    }

    template <class T> std::shared_ptr<T> retain(T* object) {
      return object == nullptr
          ? std::shared_ptr<T>{}
          : std::shared_ptr<T>(static_cast<T*>(g_object_ref(object)), [](T* value) { g_object_unref(value); });
    }

    std::string blockFamily(const std::string& source) {
      std::error_code error;
      const auto device = std::filesystem::canonical(source, error);
      if (error)
        return {};
      const auto sys = std::filesystem::canonical(std::filesystem::path("/sys/class/block") / device.filename(), error);
      if (error)
        return {};
      // Device-mapper/RAID topology needs more than a partition-parent check.
      std::filesystem::directory_iterator slaves(sys / "slaves", error);
      if (error || slaves != std::filesystem::directory_iterator{})
        return {};
      return std::filesystem::exists(sys / "partition", error) ? sys.parent_path().string() : sys.string();
    }

    bool safeEject(const Place& place, const std::vector<MountRecord>& records) {
      GDrive* drive = place.mount ? g_mount_get_drive(place.mount.get())
          : place.volume          ? g_volume_get_drive(place.volume.get())
                                  : nullptr;
      if (!drive)
        return false; // Cannot establish the physical operation's scope.
      std::set<std::string> families;
      GList* volumes = g_drive_get_volumes(drive);
      bool safe = volumes != nullptr;
      for (auto* it = volumes; it; it = it->next) {
        char* device = g_volume_get_identifier(G_VOLUME(it->data), G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        const auto family = device ? blockFamily(device) : std::string{};
        g_free(device);
        if (family.empty())
          safe = false;
        families.insert(family);
      }
      g_list_free_full(volumes, g_object_unref);
      g_object_unref(drive);
      for (const auto& record : records) {
        if (!record.source.starts_with("/dev/"))
          continue;
        const auto family = blockFamily(record.source);
        if ((record.systemCritical || protectedMount(record.path)) && (family.empty() || families.contains(family)))
          safe = false;
      }
      return safe;
    }

    void setCapabilities(Place& place, GMount* mount, GVolume* volume) {
      place.mount = retain(mount);
      if (volume != nullptr)
        place.volume = retain(volume);
      place.canMount = mount == nullptr && place.volume && g_volume_can_mount(place.volume.get());
      place.canUnmount = mount != nullptr && g_mount_can_unmount(mount);
      place.canEject =
          (mount != nullptr && g_mount_can_eject(mount)) || (place.volume && g_volume_can_eject(place.volume.get()));
    }

    bool hasOption(std::string_view options, std::string_view option) {
      while (!options.empty()) {
        const auto end = options.find(',');
        if (options.substr(0, end) == option)
          return true;
        if (end == std::string_view::npos)
          break;
        options.remove_prefix(end + 1);
      }
      return false;
    }

    const char* unmountHelper(UnixUnmountBackend backend) {
      if (backend == UnixUnmountBackend::Fuse)
        return "/usr/bin/fusermount3";
      if (backend == UnixUnmountBackend::UserMount)
        return "/usr/bin/umount";
      return nullptr;
    }

    void setUnixCapabilities(Place& place, GUnixMountEntry* mount, GList* points) {
      place.unixMount = std::shared_ptr<GUnixMountEntry>(g_unix_mount_entry_copy(mount), g_unix_mount_entry_free);
      if (place.canUnmount)
        return; // GIO remains the preferred backend.
      std::string configuredOptions;
      for (auto* it = points; it != nullptr; it = it->next) {
        auto* point = static_cast<GUnixMountPoint*>(it->data);
        if (place.path == g_unix_mount_point_get_mount_path(point)
            && std::string_view(g_unix_mount_entry_get_device_path(mount)) == g_unix_mount_point_get_device_path(point)
            && std::string_view(g_unix_mount_entry_get_fs_type(mount)) == g_unix_mount_point_get_fs_type(point)) {
          const auto* options = g_unix_mount_point_get_options(point);
          configuredOptions = options == nullptr ? "" : options;
          break;
        }
      }
      const char* options = g_unix_mount_entry_get_options(mount);
      place.unixBackend = unixUnmountBackend(
          place.path, g_unix_mount_entry_get_fs_type(mount), options == nullptr ? "" : options, configuredOptions,
          getuid(), g_file_test("/usr/bin/fusermount3", G_FILE_TEST_IS_EXECUTABLE),
          g_file_test("/usr/bin/umount", G_FILE_TEST_IS_EXECUTABLE)
      );
      place.canUnmount = place.unixBackend != UnixUnmountBackend::None;
    }

    bool usefulUnixMount(GUnixMountEntry* mount) {
      const char* rawPath = g_unix_mount_entry_get_mount_path(mount);
      const char* rawType = g_unix_mount_entry_get_fs_type(mount);
      if (rawPath == nullptr || rawType == nullptr)
        return false;
      const std::string_view path(rawPath);
      const std::string_view type(rawType);
      const char* rawSource = g_unix_mount_entry_get_device_path(mount);
      const std::string_view source = rawSource == nullptr ? std::string_view{} : std::string_view(rawSource);
      const auto mountLeaf = std::filesystem::path(path).filename().string();
      if (path.starts_with("/tmp/") && mountLeaf.starts_with(".mount_") && source.ends_with(".AppImage"))
        return false;
      if (g_unix_is_system_fs_type(rawType))
        return false;
      // Keep the host root as a useful destination. Other system-internal
      // mounts include the automatically mounted Btrfs views under /root and /var.
      const char* rawOptions = g_unix_mount_entry_get_options(mount);
      const bool hasDisplayOverride =
          rawOptions != nullptr && std::string_view(rawOptions).find("x-gvfs-show") != std::string_view::npos;
      if (path != "/" && path != "/home" && g_unix_mount_entry_is_system_internal(mount) && !hasDisplayOverride)
        return false;
      if (type == "autofs"
          || type == "rpc_pipefs"
          || type == "sunrpc"
          || type == "nfsd"
          || type == "bpf"
          || type == "nsfs"
          || type == "overlay"
          || type == "fuse.portal"
          || type == "fuse.gvfsd-fuse")
        return false;
      return true;
    }

    std::string compactMountLabel(std::string_view source, const std::filesystem::path& mountPath) {
      const auto leaf = mountPath.filename().string();
      if (!leaf.empty() && leaf != "/")
        return leaf;
      const auto colon = source.find(":/");
      if (colon != std::string_view::npos) {
        const auto remoteLeaf = std::filesystem::path(source.substr(colon + 1)).filename().string();
        if (!remoteLeaf.empty())
          return remoteLeaf;
      }
      const auto sourceLeaf = std::filesystem::path(source).filename().string();
      return sourceLeaf.empty() ? std::string("Filesystem") : sourceLeaf;
    }

    std::string unixIdentity(
        std::string_view source, std::string_view root, std::string_view type, const std::filesystem::path& mountPath
    ) {
      return "unix:"
          + std::string(source)
          + "|root:"
          + std::string(root)
          + "|type:"
          + std::string(type)
          + "|path:"
          + mountPath.lexically_normal().string();
    }

    void appendIconNames(std::vector<std::string>& names, GIcon* icon) {
      if (icon == nullptr || !G_IS_THEMED_ICON(icon))
        return;
      const gchar* const* themed = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (auto name = themed; name != nullptr && *name != nullptr; ++name) {
        if (std::ranges::find(names, *name) == names.end())
          names.emplace_back(*name);
      }
    }
  } // namespace

  void
  applyMountSafety(Place& place, const std::vector<MountRecord>& records, const std::vector<ManagedMount>& backends) {
    if (place.canEject && !safeEject(place, records))
      place.canEject = false;
    if (place.personal || !place.mounted)
      return;
    const auto record = std::ranges::find(records, place.path, &MountRecord::path);
    if (protectedMount(place.path)
        || (record != records.end() && record->systemCritical)
        || std::ranges::count(records, place.path, &MountRecord::path) > 1) {
      place.canUnmount = place.canEject = false;
      place.unixBackend = UnixUnmountBackend::None;
      return;
    }
    // GIO and UDisks filesystem operations may affect all views of one device.
    // Only the exact systemd mount-unit path is eligible for shared/Btrfs views.
    if (record != records.end() && record->source.starts_with("/dev/") && !isolatedFilesystem(*record, records)) {
      place.canUnmount = place.canEject = false;
      place.unixBackend = UnixUnmountBackend::None;
    }
    if (place.canUnmount)
      return;
    for (const auto& backend : backends) {
      if (record == records.end()
          || backend.mount.path != place.path
          || backend.mount.source != record->source
          || backend.mount.type != record->type)
        continue;
      if (backend.backend == MountBackend::UDisks && !isolatedFilesystem(*record, records))
        continue;
      place.managedMount = backend;
      place.managedMount.mount = *record;
      place.canUnmount = true;
      break;
    }
  }

  UnixUnmountBackend unixUnmountBackend(
      const std::filesystem::path& path, std::string_view type, std::string_view options,
      std::string_view configuredOptions, unsigned userId, bool fuseAvailable, bool umountAvailable
  ) {
    if (protectedMount(path))
      return UnixUnmountBackend::None;
    if ((type == "fuse" || type.starts_with("fuse.")) && fuseAvailable
        && hasOption(options, "user_id=" + std::to_string(userId)))
      return UnixUnmountBackend::Fuse;
    if (umountAvailable && (hasOption(configuredOptions, "users") || hasOption(configuredOptions, "user")))
      return UnixUnmountBackend::UserMount;
    return UnixUnmountBackend::None;
  }

  std::string_view placeGroup(const Place& place) {
    return place.personal ? "Personal" : place.network ? "Network" : "Devices";
  }

  void orderPlaces(std::vector<Place>& places, const std::vector<std::string>& order) {
    std::set<std::string> seen;
    std::erase_if(places, [&](const Place& place) { return !seen.insert(place.identity).second; });
    std::stable_sort(places.begin(), places.end(), [&](const Place& a, const Place& b) {
      if (placeGroup(a) != placeGroup(b))
        return (a.personal ? 0 : a.network ? 2 : 1) < (b.personal ? 0 : b.network ? 2 : 1);
      const auto ai = std::ranges::find(order, a.identity), bi = std::ranges::find(order, b.identity);
      if (ai != order.end() || bi != order.end())
        return ai < bi;
      if (a.mounted != b.mounted)
        return a.mounted;
      // Retain conventional XDG order until Personal is explicitly organised.
      if (a.personal)
        return false;
      const int collated = g_utf8_collate(a.label.c_str(), b.label.c_str());
      return collated == 0 ? a.identity < b.identity : collated < 0;
    });
  }

  std::vector<std::string> reorderedPlaces(
      const std::vector<Place>& places, std::vector<std::string> order, const std::string& identity,
      const std::string& before
  ) {
    const auto source = std::ranges::find(places, identity, &Place::identity);
    if (source == places.end() || identity == before)
      return order;
    const auto target = std::ranges::find(places, before, &Place::identity);
    if (target == places.end() ? before != "end:" + std::string(placeGroup(*source))
                               : placeGroup(*source) != placeGroup(*target))
      return order;
    // Keep missing identities in the saved sequence for reconnection.
    std::set<std::string> seen;
    std::erase_if(order, [&](const std::string& id) { return !seen.insert(id).second; });
    for (const auto& place : places)
      if (seen.insert(place.identity).second)
        order.push_back(place.identity);
    std::erase(order, identity);
    auto position = before.starts_with("end:") ? order.end() : std::ranges::find(order, before);
    order.insert(position, identity);
    return order;
  }

  std::string placeTooltip(const Place& place, std::string_view filesystemType, std::uint64_t free, std::uint64_t total) {
    std::string text = place.label;
    auto line = [&](const std::string& value) {
      if (!value.empty()) {
        if (!text.empty())
          text += "\n";
        text += value;
      }
    };
    if (!place.mounted) {
      line("Not mounted");
      return text;
    }
    const auto type = place.filesystemType.empty() ? filesystemType : std::string_view(place.filesystemType);
    std::string location = place.path.string();
    auto field = [&](std::string_view value) {
      if (!value.empty()) {
        if (!location.empty())
          location += " · ";
        location += value;
      }
    };
    field(type == "btrfs" ? "Btrfs" : type);
    if (place.network || type.starts_with("fuse"))
      field(place.mountSource);
    line(location);
    if (total > 0) {
      gchar* freeText = g_format_size_full(free, G_FORMAT_SIZE_IEC_UNITS);
      gchar* totalText = g_format_size_full(total, G_FORMAT_SIZE_IEC_UNITS);
      if (freeText != nullptr && totalText != nullptr)
        line(std::string(freeText) + " free of " + totalText);
      g_free(freeText);
      g_free(totalText);
    }
    return text;
  }

  PlacesModel::PlacesModel(bool backendDiscovery) : m_backendDiscovery(backendDiscovery) {
    m_volumeMonitor = g_volume_monitor_get();
    m_unixMountMonitor = g_unix_mount_monitor_get();
    for (const char* signal :
         {"drive-connected", "drive-disconnected", "drive-changed", "volume-added", "volume-removed", "volume-changed",
          "mount-added", "mount-removed", "mount-changed"}) {
      m_volumeSignals.push_back(g_signal_connect(m_volumeMonitor, signal, G_CALLBACK(volumeMonitorChanged), this));
    }
    for (const char* signal : {"mounts-changed", "mountpoints-changed"}) {
      m_unixSignals.push_back(g_signal_connect(m_unixMountMonitor, signal, G_CALLBACK(monitorChanged), this));
    }
    if (m_backendDiscovery) {
      auto appeared = +[](GDBusConnection*, const char*, const char*, gpointer data) {
        static_cast<PlacesModel*>(data)->refreshBackends();
      };
      auto vanished = +[](GDBusConnection*, const char*, gpointer data) {
        auto* self = static_cast<PlacesModel*>(data);
        self->m_managedMounts.clear();
        self->scheduleChanged();
      };
      m_udisksWatch = g_bus_watch_name(
          G_BUS_TYPE_SYSTEM, "org.freedesktop.UDisks2", G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, this, nullptr
      );
      m_systemdWatch = g_bus_watch_name(
          G_BUS_TYPE_SYSTEM, "org.freedesktop.systemd1", G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, this,
          nullptr
      );
      refreshBackends();
    }
  }

  PlacesModel::~PlacesModel() {
    *m_alive = false;
    if (m_udisksWatch)
      g_bus_unwatch_name(m_udisksWatch);
    if (m_systemdWatch)
      g_bus_unwatch_name(m_systemdWatch);
    if (m_changedSource != 0)
      g_source_remove(m_changedSource);
    for (const auto signal : m_volumeSignals)
      if (signal != 0 && g_signal_handler_is_connected(m_volumeMonitor, signal))
        g_signal_handler_disconnect(m_volumeMonitor, signal);
    for (const auto signal : m_unixSignals)
      if (signal != 0 && g_signal_handler_is_connected(m_unixMountMonitor, signal))
        g_signal_handler_disconnect(m_unixMountMonitor, signal);
    g_clear_object(&m_volumeMonitor);
    g_clear_object(&m_unixMountMonitor);
  }

  void PlacesModel::setChangedCallback(Changed callback) { m_changed = std::move(callback); }

  void PlacesModel::monitorChanged(GObject*, gpointer data) {
    auto* self = static_cast<PlacesModel*>(data);
    self->refreshBackends();
    self->scheduleChanged();
  }

  void PlacesModel::volumeMonitorChanged(GVolumeMonitor*, GObject*, gpointer data) { monitorChanged(nullptr, data); }

  void PlacesModel::refreshBackends() {
    if (!m_backendDiscovery)
      return;
    if (m_refreshing) {
      m_refreshAgain = true;
      return;
    }
    m_refreshing = true;
    discoverManagedMounts([this, alive = m_alive](std::vector<ManagedMount> mounts) {
      if (!*alive)
        return;
      m_refreshing = false;
      m_managedMounts = std::move(mounts);
      scheduleChanged();
      if (m_refreshAgain) {
        m_refreshAgain = false;
        refreshBackends();
      }
    });
  }

  void PlacesModel::scheduleChanged() {
    if (m_changedSource == 0)
      m_changedSource = g_idle_add(dispatchChanged, this);
  }

  gboolean PlacesModel::dispatchChanged(gpointer data) {
    auto* self = static_cast<PlacesModel*>(data);
    self->m_changedSource = 0;
    if (self->m_changed)
      self->m_changed();
    return G_SOURCE_REMOVE;
  }

  std::vector<Place> PlacesModel::discover() const {
    std::vector<Place> result;
    std::set<std::string> personalPaths;
    const char* home = g_get_home_dir();
    if (home != nullptr && *home != '\0') {
      const auto normalizedHome = std::filesystem::path(home).lexically_normal();
      result.push_back(
          {.label = "Home",
           .identity = "personal:home:" + normalizedHome.string(),
           .path = normalizedHome,
           .iconNames = {"user-home", "folder"},
           .network = false,
           .mounted = true,
           .personal = true}
      );
      std::error_code ec;
      const auto target = std::filesystem::canonical(normalizedHome, ec);
      personalPaths.insert(ec ? normalizedHome.string() : target.string());
    }
    const std::pair<GUserDirectory, const char*> directories[] = {
        {G_USER_DIRECTORY_DESKTOP, "Desktop"},    {G_USER_DIRECTORY_DOCUMENTS, "Documents"},
        {G_USER_DIRECTORY_DOWNLOAD, "Downloads"}, {G_USER_DIRECTORY_MUSIC, "Music"},
        {G_USER_DIRECTORY_PICTURES, "Pictures"},  {G_USER_DIRECTORY_VIDEOS, "Videos"}
    };
    for (const auto& [kind, fallback] : directories) {
      const char* rawPath = g_get_user_special_dir(kind);
      if (rawPath == nullptr || *rawPath == '\0')
        continue;
      const auto path = std::filesystem::path(rawPath).lexically_normal();
      // canonical() follows only this explicitly configured Personal location;
      // filesystem discovery itself must not recurse through symlinks.
      std::error_code ec;
      const auto target = std::filesystem::canonical(path, ec);
      if (ec || !std::filesystem::is_directory(target, ec) || ec)
        continue;
      if (!personalPaths.insert(target.string()).second)
        continue;
      result.push_back(
          {.label = fallback,
           .identity = "personal:" + std::string(fallback) + ":" + path.string(),
           .path = path,
           .iconNames =
               {kind == G_USER_DIRECTORY_DESKTOP         ? "user-desktop"
                    : kind == G_USER_DIRECTORY_DOCUMENTS ? "folder-documents"
                    : kind == G_USER_DIRECTORY_DOWNLOAD  ? "folder-download"
                    : kind == G_USER_DIRECTORY_MUSIC     ? "folder-music"
                    : kind == G_USER_DIRECTORY_PICTURES  ? "folder-pictures"
                                                         : "folder-videos",
                "folder"},
           .network = false,
           .mounted = true,
           .personal = true}
      );
    }

    auto findMountedPath = [&](const std::filesystem::path& path) {
      return std::ranges::find_if(result, [&](const Place& place) {
        return place.mounted && !place.personal && place.path.lexically_normal() == path.lexically_normal();
      });
    };
    auto addGioMount = [&](GMount* mount, GVolume* volume = nullptr) {
      GVolume* ownedVolume = volume == nullptr ? g_mount_get_volume(mount) : nullptr;
      if (ownedVolume != nullptr)
        volume = ownedVolume;
      GFile* root = g_mount_get_root(mount);
      char* rawPath = root == nullptr ? nullptr : g_file_get_path(root);
      if (rawPath != nullptr) {
        const std::filesystem::path path(rawPath);
        const auto existing = findMountedPath(path);
        char* name = volume != nullptr ? g_volume_get_name(volume) : g_mount_get_name(mount);
        char* uuid = volume != nullptr ? g_volume_get_uuid(volume) : g_mount_get_uuid(mount);
        char* device =
            volume == nullptr ? nullptr : g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        const std::string identity = uuid != nullptr && *uuid != '\0' ? "volume:" + std::string(uuid)
            : device != nullptr                                       ? "volume-device:" + std::string(device)
            : volume != nullptr && name != nullptr                    ? "volume-name:" + std::string(name)
                                                                      : "gio-mount:" + path.lexically_normal().string();
        g_free(device);
        GIcon* icon = volume != nullptr ? g_volume_get_icon(volume) : g_mount_get_icon(mount);
        std::vector<std::string> icons;
        appendIconNames(icons, icon);
        if (icon != nullptr)
          g_object_unref(icon);
        char* uri = g_file_get_uri(root);
        const std::string details = uri == nullptr ? path.string() : std::string(uri) + " · " + path.string();
        if (existing != result.end()) {
          if (name != nullptr && *name != '\0')
            existing->label = name;
          if (!icons.empty())
            existing->iconNames = std::move(icons);
          existing->details = details;
          existing->identity = identity;
          existing->capacityIdentity = uuid != nullptr && *uuid != '\0' ? "giofs:" + std::string(uuid)
                                                                        : "giofs:" + path.lexically_normal().string();
        } else {
          const std::string label = name != nullptr && *name != '\0' ? name : compactMountLabel({}, path);
          result.push_back(
              {.label = label,
               .identity = identity,
               .capacityIdentity = uuid != nullptr && *uuid != '\0' ? "giofs:" + std::string(uuid)
                                                                    : "giofs:" + path.lexically_normal().string(),
               .details = details,
               .path = path,
               .iconNames = std::move(icons),
               .network = false,
               .mounted = true,
               .personal = false}
          );
        }
        auto added = findMountedPath(path);
        if (added != result.end())
          setCapabilities(*added, mount, volume);
        g_free(uri);
        g_free(name);
        g_free(uuid);
        g_free(rawPath);
      }
      if (root != nullptr)
        g_object_unref(root);
      g_clear_object(&ownedVolume);
    };

    GList* volumes = g_volume_monitor_get_volumes(m_volumeMonitor);
    for (GList* it = volumes; it != nullptr; it = it->next) {
      auto* volume = G_VOLUME(it->data);
      GMount* mount = g_volume_get_mount(volume);
      char* name = g_volume_get_name(volume);
      char* uuid = g_volume_get_uuid(volume);
      if (mount != nullptr) {
        addGioMount(mount, volume);
        g_object_unref(mount);
      } else if (name != nullptr && *name != '\0') {
        GIcon* icon = g_volume_get_icon(volume);
        std::vector<std::string> icons;
        appendIconNames(icons, icon);
        if (icon != nullptr)
          g_object_unref(icon);
        char* device = g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
        const std::string identity = uuid != nullptr && *uuid != '\0' ? "volume:" + std::string(uuid)
            : device != nullptr                                       ? "volume-device:" + std::string(device)
                                                                      : "volume-name:" + std::string(name);
        g_free(device);
        if (std::ranges::none_of(result, [&](const Place& place) { return place.identity == identity; })) {
          result.push_back(
              {.label = name,
               .identity = identity,
               .details = name,
               .iconNames = std::move(icons),
               .network = false,
               .mounted = false,
               .personal = false}
          );
          setCapabilities(result.back(), nullptr, volume);
        }
      }
      g_free(name);
      g_free(uuid);
    }
    g_list_free_full(volumes, g_object_unref);

    GList* gioMounts = g_volume_monitor_get_mounts(m_volumeMonitor);
    for (GList* it = gioMounts; it != nullptr; it = it->next)
      addGioMount(G_MOUNT(it->data));
    g_list_free_full(gioMounts, g_object_unref);

    GList* points = g_unix_mount_points_get(nullptr);
    GList* mounts = g_unix_mount_entries_get(nullptr);
    for (GList* it = mounts; it != nullptr; it = it->next) {
      auto* mount = static_cast<GUnixMountEntry*>(it->data);
      if (!usefulUnixMount(mount))
        continue;
      const char* rawPath = g_unix_mount_entry_get_mount_path(mount);
      const char* rawSource = g_unix_mount_entry_get_device_path(mount);
      const char* rawType = g_unix_mount_entry_get_fs_type(mount);
      const char* rawRoot = g_unix_mount_entry_get_root_path(mount);
      const char* rawOptions = g_unix_mount_entry_get_options(mount);
      const std::string source = rawSource != nullptr ? rawSource : "";
      const std::string type = rawType != nullptr ? rawType : "";
      const std::string root = rawRoot != nullptr ? rawRoot : "/";
      const auto path = std::filesystem::path(rawPath).lexically_normal();
      const bool network = networkFs(type, source);
      if ((type == "nfs" || type == "nfs4") && source.find(":/") == std::string::npos)
        continue;
      const std::string identity = unixIdentity(source, root, type, path);
      auto existing = findMountedPath(path);
      GIcon* guessedIcon = g_unix_mount_entry_guess_icon(mount);
      std::vector<std::string> mountIcons;
      appendIconNames(mountIcons, guessedIcon);
      if (guessedIcon != nullptr)
        g_object_unref(guessedIcon);
      char* guessed = g_unix_mount_entry_guess_name(mount);
      const auto volumeLabel =
          source.starts_with("/dev/") && (path == "/" || path == "/home") ? filesystemLabel(source) : std::string{};
      if (existing != result.end()) {
        setUnixCapabilities(*existing, mount, points);
        existing->network = network;
        existing->filesystemType = type;
        existing->mountSource = source;
        existing->details = source
            + " · "
            + path.string()
            + " · "
            + type
            + (rawOptions == nullptr || *rawOptions == '\0' ? "" : " · " + std::string(rawOptions));
        if (network)
          existing->label = compactMountLabel(source, path);
        if (existing->iconNames.empty())
          existing->iconNames = std::move(mountIcons);
        if (existing->identity.empty())
          existing->identity = identity;
        // A real GIO volume name or a user-supplied mount name wins. Replace
        // only the same generic name guessed from this Unix mount.
        if (!volumeLabel.empty() && !existing->volume && guessed != nullptr && existing->label == guessed)
          existing->label = volumeLabel;
        g_free(guessed);
        continue;
      }
      std::string label = compactMountLabel(source, path);
      if (guessed != nullptr
          && *guessed != '\0'
          && source.find(":/") == std::string::npos
          && std::string_view(guessed) != source
          && !std::string_view(guessed).starts_with("/dev/"))
        label = guessed;
      if (!volumeLabel.empty())
        label = volumeLabel;
      const std::string options = rawOptions != nullptr ? rawOptions : "";
      const std::string details =
          source + " · " + path.string() + " · " + type + (options.empty() ? "" : " · " + options);
      result.push_back(
          {.label = std::move(label),
           .identity = identity,
           .capacityIdentity = source + "|" + type,
           .details = details,
           .filesystemType = type,
           .mountSource = source,
           .path = path,
           .iconNames = std::move(mountIcons),
           .network = network,
           .mounted = true,
           .personal = false}
      );
      setUnixCapabilities(result.back(), mount, points);
      g_free(guessed);
    }
    g_list_free_full(points, reinterpret_cast<GDestroyNotify>(g_unix_mount_point_free));
    g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
    const auto records = mountRecords();
    // A volume monitor can lag Unix discovery. Do not offer Mount for a device
    // already mounted through another provider (including device aliases).
    std::erase_if(result, [&](const Place& place) {
      if (place.mounted || !place.volume)
        return false;
      char* rawDevice = g_volume_get_identifier(place.volume.get(), G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
      if (!rawDevice)
        return false;
      std::error_code error;
      auto device = std::filesystem::canonical(rawDevice, error).string();
      if (error)
        device = rawDevice;
      g_free(rawDevice);
      return std::ranges::any_of(records, [&](const auto& record) { return record.source == device; });
    });
    for (auto& place : result)
      applyMountSafety(place, records, m_managedMounts);
    orderPlaces(result, {});
    return result;
  }

  void PlacesModel::operate(
      const Place& place, PlaceOperation operation, GMountOperation* prompts, GCancellable* cancellable,
      std::function<void(std::string)> completion
  ) {
    static bool active = false;
    if (active) {
      completion("Wait for the current device operation to finish.");
      return;
    }
    active = true;
    completion = [done = std::move(completion)](std::string message) {
      active = false;
      done(std::move(message));
    };
    struct Request {
      Place place;
      PlaceOperation operation;
      std::shared_ptr<GMountOperation> prompts;
      std::function<void(std::string)> completion;
    };
    if ((operation == PlaceOperation::Mount && !place.canMount)
        || (operation == PlaceOperation::Unmount && !place.canUnmount)
        || (operation == PlaceOperation::Eject && !place.canEject)) {
      completion("This device no longer supports the requested operation.");
      return;
    }
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) {
      completion("Device operation cancelled.");
      return;
    }
    PlacesModel currentModel(false);
    const auto currentPlaces = currentModel.discover();
    const auto current = std::ranges::find(currentPlaces, place.identity, &Place::identity);
    if (current == currentPlaces.end() || current->mounted != place.mounted || current->path != place.path
        || (place.unixMount && (!current->unixMount
            || g_unix_mount_entry_compare(place.unixMount.get(), current->unixMount.get()) != 0))) {
      completion("The device or mount has changed. Open its menu again.");
      return;
    }
    if (operation != PlaceOperation::Mount && current->mounted && protectedMount(current->path)) {
      completion("This filesystem is protected because it contains system paths.");
      return;
    }
    if (operation == PlaceOperation::Unmount
        && !current->canUnmount
        && place.managedMount.backend != MountBackend::None) {
      operateManagedMount(place.managedMount, cancellable, std::move(completion));
      return;
    }
    if (operation == PlaceOperation::Unmount && current->unixBackend != UnixUnmountBackend::None) {
      const auto arguments = unmountArguments(current->unixBackend, current->path);
      std::vector<const char*> argv;
      for (const auto& argument : arguments)
        argv.push_back(argument.c_str());
      argv.push_back(nullptr);
      GError* error = nullptr;
      auto* process = g_subprocess_newv(
          argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error
      );
      if (process == nullptr) {
        const std::string message = error == nullptr ? "Unable to start unmount helper." : error->message;
        g_clear_error(&error);
        completion(message);
        return;
      }
      waitForUnmount(process, std::move(completion));
      g_object_unref(process);
      return;
    }
    // Recheck capabilities and object presence; never dispatch through stale/null objects.
    const bool supported = operation == PlaceOperation::Mount ? current->canMount && current->volume
        : operation == PlaceOperation::Unmount ? current->canUnmount && current->mount
        : current->canEject && (current->mount || current->volume);
    if (!supported) {
      completion("This device no longer supports the requested operation.");
      return;
    }
    auto* request = new Request{*current, operation, retain(prompts), std::move(completion)};
    auto ready = +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Request> completed(static_cast<Request*>(data));
      GError* error = nullptr;
      switch (completed->operation) {
      case PlaceOperation::Mount:
        g_volume_mount_finish(G_VOLUME(source), result, &error);
        break;
      case PlaceOperation::Unmount:
        g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
        break;
      case PlaceOperation::Eject:
        if (G_IS_MOUNT(source))
          g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error);
        else
          g_volume_eject_with_operation_finish(G_VOLUME(source), result, &error);
        break;
      }
      // A competing request may have mounted this volume while our menu was open.
      const bool alreadyMounted = completed->operation == PlaceOperation::Mount
          && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
      const std::string message = error != nullptr && !alreadyMounted ? error->message : "";
      g_clear_error(&error);
      completed->completion(message);
    };
    switch (operation) {
    case PlaceOperation::Mount:
      g_volume_mount(request->place.volume.get(), G_MOUNT_MOUNT_NONE, prompts, cancellable, ready, request);
      break;
    case PlaceOperation::Unmount:
      g_mount_unmount_with_operation(request->place.mount.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      break;
    case PlaceOperation::Eject:
      if (request->place.mount && g_mount_can_eject(request->place.mount.get()))
        g_mount_eject_with_operation(request->place.mount.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      else
        g_volume_eject_with_operation(request->place.volume.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      break;
    }
  }

  std::vector<std::string> PlacesModel::unmountArguments(
      UnixUnmountBackend backend, const std::filesystem::path& path
  ) {
    // Argument vector only: never a shell, force/lazy flags or elevated privileges.
    if (backend == UnixUnmountBackend::Fuse)
      return {unmountHelper(backend), "-u", "--", path.string()};
    if (backend == UnixUnmountBackend::UserMount)
      return {unmountHelper(backend), "--", path.string()};
    return {};
  }

  void PlacesModel::waitForUnmount(GSubprocess* process, std::function<void(std::string)> completion) {
    // Always reap the helper before releasing the operation guard, even if the
    // caller cancels. Cancellation must never kill a mount helper mid-operation.
    auto* done = new std::function<void(std::string)>(std::move(completion));
    g_subprocess_communicate_utf8_async(
        process, nullptr, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<std::function<void(std::string)>> callback(
              static_cast<std::function<void(std::string)>*>(data)
          );
          GError* error = nullptr;
          char* errors = nullptr;
          const bool completed =
              g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, nullptr, &errors, &error);
          std::string message;
          if (!completed)
            message = error == nullptr ? "Unable to wait for unmount helper." : error->message;
          else if (!g_subprocess_get_successful(G_SUBPROCESS(source))) {
            char* lower = g_ascii_strdown(errors ? errors : "", -1);
            const std::string_view diagnostic(lower);
            message = diagnostic.find("busy") != std::string_view::npos
                ? "This device is busy. Close open files and try again."
                : diagnostic.find("permission") != std::string_view::npos
                ? "Permission to unmount this filesystem was denied."
                : "Unmount failed. The device may be disconnected or restricted by system policy.";
            g_free(lower);
          }
          g_free(errors);
          g_clear_error(&error);
          (*callback)(std::move(message));
        },
        done
    );
  }

  void PlacesModel::queryCapacity(
      const std::filesystem::path& path, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer userData
  ) {
    GFile* file = g_file_new_for_path(path.c_str());
    g_file_query_filesystem_info_async(
        file,
        G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE
                                       "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
        G_PRIORITY_DEFAULT, cancellable, callback, userData
    );
    g_object_unref(file);
  }
} // namespace filemanager
