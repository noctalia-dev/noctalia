#include "filemanager/places_model.h"

#include <algorithm>
#include <gio/gunixmounts.h>
#include <ranges>
#include <set>
#include <string_view>

namespace filemanager {
  namespace {
    bool networkFs(std::string_view type, std::string_view source) {
      if (type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3" || type == "sshfs"
          || type == "fuse.sshfs" || type == "fuse.rclone")
        return true;
      return source.find(":/") != std::string_view::npos;
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
      const bool hasDisplayOverride = rawOptions != nullptr && std::string_view(rawOptions).find("x-gvfs-show") != std::string_view::npos;
      if (path != "/" && path != "/home" && g_unix_mount_entry_is_system_internal(mount) && !hasDisplayOverride)
        return false;
      if (type == "autofs" || type == "rpc_pipefs" || type == "sunrpc" || type == "nfsd" || type == "bpf"
          || type == "nsfs" || type == "overlay" || type == "fuse.portal" || type == "fuse.gvfsd-fuse")
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

    std::string unixIdentity(std::string_view source, std::string_view root, std::string_view type,
        const std::filesystem::path& mountPath) {
      return "unix:" + std::string(source) + "|root:" + std::string(root) + "|type:" + std::string(type)
          + "|path:" + mountPath.lexically_normal().string();
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

  PlacesModel::PlacesModel() {
    m_volumeMonitor = g_volume_monitor_get();
    m_unixMountMonitor = g_unix_mount_monitor_get();
    for (const char* signal : {"drive-connected", "drive-disconnected", "drive-changed", "volume-added",
             "volume-removed", "volume-changed", "mount-added", "mount-removed", "mount-changed"}) {
      m_volumeSignals.push_back(g_signal_connect(m_volumeMonitor, signal, G_CALLBACK(monitorChanged), this));
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
      result.push_back({.label="Home", .identity="personal:home:" + normalizedHome.string(), .path=normalizedHome,
          .network=false, .mounted=true, .personal=true});
      personalPaths.insert(normalizedHome.string());
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
      if (!personalPaths.insert(path.string()).second)
        continue;
      result.push_back({.label=fallback, .identity="personal:" + std::string(fallback) + ":" + path.string(),
          .path=path, .network=false, .mounted=true, .personal=true});
    }

    auto findMountedPath = [&](const std::filesystem::path& path) {
      return std::ranges::find_if(result, [&](const Place& place) {
        return place.mounted && !place.personal && place.path.lexically_normal() == path.lexically_normal();
      });
    };
    auto addGioMount = [&](GMount* mount, GVolume* volume = nullptr) {
      GFile* root = g_mount_get_root(mount);
      char* rawPath = root == nullptr ? nullptr : g_file_get_path(root);
      if (rawPath != nullptr) {
        const std::filesystem::path path(rawPath);
        const auto existing = findMountedPath(path);
        char* name = volume != nullptr ? g_volume_get_name(volume) : g_mount_get_name(mount);
        char* uuid = volume != nullptr ? g_volume_get_uuid(volume) : g_mount_get_uuid(mount);
        GIcon* icon = volume != nullptr ? g_volume_get_icon(volume) : g_mount_get_icon(mount);
        std::vector<std::string> icons;
        appendIconNames(icons, icon);
        char* uri = g_file_get_uri(root);
        const std::string details = uri == nullptr ? path.string() : std::string(uri) + " · " + path.string();
        if (existing != result.end()) {
          if (name != nullptr && *name != '\0')
            existing->label = name;
          if (!icons.empty())
            existing->iconNames = std::move(icons);
          existing->details = details;
          if (uuid != nullptr && *uuid != '\0')
            existing->identity = "gio:" + std::string(uuid) + "|path:" + path.lexically_normal().string();
          existing->capacityIdentity = uuid != nullptr && *uuid != '\0' ? "giofs:" + std::string(uuid)
                                                                         : "giofs:" + path.lexically_normal().string();
        } else {
          const std::string label = name != nullptr && *name != '\0' ? name : compactMountLabel({}, path);
          const std::string identity = uuid != nullptr && *uuid != '\0'
              ? "gio:" + std::string(uuid) + "|path:" + path.lexically_normal().string()
              : "gio-mount:" + path.lexically_normal().string();
          result.push_back({.label=label, .identity=identity,
              .capacityIdentity=uuid != nullptr && *uuid != '\0' ? "giofs:" + std::string(uuid)
                                                                  : "giofs:" + path.lexically_normal().string(),
              .details=details, .path=path,
              .iconNames=std::move(icons), .network=false, .mounted=true, .personal=false});
        }
        g_free(uri);
        g_free(name);
        g_free(uuid);
        g_free(rawPath);
      }
      if (root != nullptr)
        g_object_unref(root);
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
        const std::string identity = "volume:" + std::string(uuid != nullptr ? uuid : "") + "|name:" + name;
        if (std::ranges::none_of(result, [&](const Place& place) { return place.identity == identity; }))
          result.push_back({.label=name, .identity=identity, .details=name, .iconNames=std::move(icons),
              .network=false, .mounted=false, .personal=false});
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
      if ((type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3")
          && source.find(":/") == std::string::npos)
        continue;
      const std::string identity = unixIdentity(source, root, type, path);
      auto existing = findMountedPath(path);
      GIcon* guessedIcon = g_unix_mount_entry_guess_icon(mount);
      std::vector<std::string> mountIcons;
      appendIconNames(mountIcons, guessedIcon);
      if (guessedIcon != nullptr) g_object_unref(guessedIcon);
      if (existing != result.end()) {
        existing->network = network;
        existing->details = source + " · " + path.string() + " · " + type
            + (rawOptions == nullptr || *rawOptions == '\0' ? "" : " · " + std::string(rawOptions));
        if (network) existing->label = compactMountLabel(source, path);
        if (existing->iconNames.empty()) existing->iconNames = std::move(mountIcons);
        if (existing->identity.empty())
          existing->identity = identity;
        continue;
      }
      char* guessed = g_unix_mount_entry_guess_name(mount);
      std::string label = compactMountLabel(source, path);
      if (guessed != nullptr && *guessed != '\0' && source.find(":/") == std::string::npos
          && std::string_view(guessed) != source && !std::string_view(guessed).starts_with("/dev/"))
        label = guessed;
      const std::string options = rawOptions != nullptr ? rawOptions : "";
      const std::string details = source + " · " + path.string() + " · " + type
          + (options.empty() ? "" : " · " + options);
      result.push_back({.label=std::move(label), .identity=identity, .capacityIdentity=source + "|" + type,
          .details=details, .path=path, .iconNames=std::move(mountIcons),
          .network=network, .mounted=true, .personal=false});
      g_free(guessed);
    }
    g_list_free_full(mounts, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
    return result;
  }

  void PlacesModel::queryCapacity(
      const std::filesystem::path& path, GCancellable* cancellable, GAsyncReadyCallback callback, gpointer userData
  ) {
    GFile* file = g_file_new_for_path(path.c_str());
    g_file_query_filesystem_info_async(
        file,
        G_FILE_ATTRIBUTE_ID_FILESYSTEM "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE ","
        G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
        G_PRIORITY_DEFAULT, cancellable, callback, userData
    );
    g_object_unref(file);
  }
} // namespace filemanager
