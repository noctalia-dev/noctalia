#include "filemanager/places_model.h"

#include "filemanager/place_metadata.h"

#include <algorithm>
#include <gio/gunixmounts.h>
#include <ranges>
#include <set>
#include <string_view>

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

    void setCapabilities(Place& place, GMount* mount, GVolume* volume) {
      place.mount = retain(mount);
      if (volume != nullptr)
        place.volume = retain(volume);
      place.canMount = mount == nullptr && place.volume && g_volume_can_mount(place.volume.get());
      place.canUnmount = mount != nullptr && g_mount_can_unmount(mount);
      place.canEject =
          (mount != nullptr && g_mount_can_eject(mount)) || (place.volume && g_volume_can_eject(place.volume.get()));
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

  PlacesModel::PlacesModel() {
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
  }

  PlacesModel::~PlacesModel() {
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

  void PlacesModel::monitorChanged(GObject*, gpointer data) { static_cast<PlacesModel*>(data)->scheduleChanged(); }

  void PlacesModel::volumeMonitorChanged(GVolumeMonitor*, GObject*, gpointer data) {
    static_cast<PlacesModel*>(data)->scheduleChanged();
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
      g_free(guessed);
    }
    g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
    orderPlaces(result, {});
    return result;
  }

  void PlacesModel::operate(
      const Place& place, PlaceOperation operation, GMountOperation* prompts, GCancellable* cancellable,
      std::function<void(std::string)> completion
  ) {
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
    auto* request = new Request{place, operation, retain(prompts), std::move(completion)};
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
      g_volume_mount(place.volume.get(), G_MOUNT_MOUNT_NONE, prompts, cancellable, ready, request);
      break;
    case PlaceOperation::Unmount:
      g_mount_unmount_with_operation(place.mount.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      break;
    case PlaceOperation::Eject:
      if (place.mount && g_mount_can_eject(place.mount.get()))
        g_mount_eject_with_operation(place.mount.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      else
        g_volume_eject_with_operation(place.volume.get(), G_MOUNT_UNMOUNT_NONE, prompts, cancellable, ready, request);
      break;
    }
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
