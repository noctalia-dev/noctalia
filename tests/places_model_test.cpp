#include "filemanager/places_model.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace filemanager {
  class PlacesModelTestAccess {
  public:
    static auto arguments(UnixUnmountBackend backend, const std::filesystem::path& path) {
      return PlacesModel::unmountArguments(backend, path);
    }
    static void wait(GSubprocess* process, std::function<void(std::string)> done) {
      PlacesModel::waitForUnmount(process, std::move(done));
    }
  };
}

int main() {
  {
    filemanager::Place device{
        .label = "Home", .details = "rw,relatime,ssd,compress=zstd:3,subvolid=256",
        .filesystemType = "btrfs", .mountSource = "/dev/test", .path = "/home"
    };
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    const auto tooltip = filemanager::placeTooltip(device, {}, 68 * gib, 176 * gib);
    assert(tooltip.starts_with("Home\n/home · Btrfs\n"));
    assert(tooltip.find("GiB free of ") != std::string::npos);
    assert(tooltip.find("176") != std::string::npos);
    assert(tooltip.find("relatime") == std::string::npos && tooltip.find("/dev/test") == std::string::npos);
    assert(filemanager::placeTooltip(device) == "Home\n/home · Btrfs");
    device.mounted = false;
    assert(filemanager::placeTooltip(device, {}, 68 * gib, 176 * gib) == "Home\nNot mounted");
    device = {.label = "Unknown"};
    assert(filemanager::placeTooltip(device) == "Unknown");
    assert(filemanager::placeTooltip(device, "ext4").ends_with("\next4"));
    device = {.label = "Share", .filesystemType = "nfs4", .mountSource = "server:/share", .path = "/mnt/share", .network = true};
    assert(filemanager::placeTooltip(device) == "Share\n/mnt/share · nfs4 · server:/share");
    device.network = false;
    device.filesystemType = "fuse.rclone";
    device.mountSource = "remote:folder";
    assert(filemanager::placeTooltip(device).ends_with(" · fuse.rclone · remote:folder"));
    assert(filemanager::placeTooltip(device, {}, 0, gib).find("free of") != std::string::npos);
  }
  char* raw = g_dir_make_tmp("nythera-places-test-XXXXXX", nullptr);
  assert(raw != nullptr);
  const std::filesystem::path root(raw);
  g_free(raw);
  std::filesystem::create_directories(root / "config");
  std::filesystem::create_directories(root / "actual-pictures");
  std::filesystem::create_directory_symlink(root / "actual-pictures", root / "Pictures");
  std::filesystem::create_directory_symlink(root / "missing", root / "Videos");
  setenv("HOME", root.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
  {
    std::ofstream config(root / "config/user-dirs.dirs");
    config
        << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
        << "XDG_VIDEOS_DIR=\"$HOME/Videos\"\n"
        << "XDG_DOCUMENTS_DIR=\"$HOME/actual-pictures\"\n"
        << "XDG_MUSIC_DIR=\"$HOME/\"\n";
  }
  filemanager::PlacesModel model;
  bool discoveryChanged = false;
  model.setChangedCallback([&] { discoveryChanged = true; });
  GVolumeMonitor* monitor = g_volume_monitor_get();
  // Volume monitor signals carry a device argument in addition to the sender.
  // Unix mount-monitor signals do not; both must reach the right model owner.
  g_signal_emit_by_name(monitor, "volume-changed", nullptr);
  while (g_main_context_iteration(nullptr, false)) {
  }
  assert(discoveryChanged);
  g_object_unref(monitor);
  const auto places = model.discover();
  const auto pictures = std::ranges::find_if(places, [](const auto& place) {
    return place.personal && (place.label == "Pictures" || place.label == "Documents");
  });
  assert(pictures != places.end());
  assert(!pictures->iconNames.empty() && pictures->iconNames.back() == "folder");
  const auto homePlace = std::ranges::find_if(places, [](const auto& place) { return place.label == "Home"; });
  assert(homePlace != places.end() && homePlace->iconNames.front() == "user-home");
  // Documents and Pictures point to the same target and must appear only once.
  assert(
      std::ranges::count_if(
          places,
          [](const auto& place) { return place.personal && (place.label == "Pictures" || place.label == "Documents"); }
      )
      == 1
  );
  assert(std::ranges::none_of(places, [](const auto& place) {
    return place.personal && (place.label == "Videos" || place.label == "Music");
  }));
  assert(!std::filesystem::exists(root / "missing"));
  for (const auto group : {"Devices", "Network"}) {
    std::vector<std::pair<bool, std::string>> labels;
    for (const auto& place : places)
      if (filemanager::placeGroup(place) == group)
        labels.emplace_back(place.mounted, place.label);
    assert(std::is_sorted(labels.begin(), labels.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first : g_utf8_collate(a.second.c_str(), b.second.c_str()) < 0;
    }));
  }
  bool rejected = false;
  filemanager::PlacesModel::operate({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [&](std::string message) {
    rejected = !message.empty();
  });
  assert(rejected);
  using filemanager::UnixUnmountBackend;
  const auto backend = [](std::string_view path, std::string_view type, std::string_view options,
                          std::string_view configured = "", bool fuse = true, bool umount = true) {
    return filemanager::unixUnmountBackend(path, type, options, configured, 1000, fuse, umount);
  };
  assert(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1000,group_id=1000") == UnixUnmountBackend::Fuse);
  assert(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1001") == UnixUnmountBackend::None);
  assert(backend("/mnt/remote", "fuse.rclone", "rw,user_id=10000") == UnixUnmountBackend::None);
  assert(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1000", "", false) == UnixUnmountBackend::None);
  assert(backend("/mnt/share", "nfs4", "rw", "rw,users,_netdev") == UnixUnmountBackend::UserMount);
  assert(backend("/mnt/disk", "ext4", "rw", "user,noauto") == UnixUnmountBackend::UserMount);
  assert(backend("/mnt/share", "nfs4", "rw", "rw,_netdev") == UnixUnmountBackend::None);
  assert(backend("/mnt/share", "nfs4", "rw", "x-users") == UnixUnmountBackend::None);
  assert(backend("/mnt/share", "nfs4", "rw", "users", true, false) == UnixUnmountBackend::None);
  for (const auto path : {"/", "/home", "/boot", "/var/lib/data", "/run/service", "relative"})
    assert(backend(path, "ext4", "rw", "users") == UnixUnmountBackend::None);
  assert(backend("/run/media/user/disk", "ext4", "rw", "users") == UnixUnmountBackend::UserMount);
  // Production dispatch keeps unusual mount paths in one inert argument.
  const std::string unusualPath = "/mnt/a share;$(false)";
  assert((filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::Fuse, unusualPath)
      == std::vector<std::string>{"/usr/bin/fusermount3", "-u", "--", unusualPath}));
  assert((filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::UserMount, unusualPath)
      == std::vector<std::string>{"/usr/bin/umount", "--", unusualPath}));
  assert(filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::None, unusualPath).empty());
  // Controlled helpers only: no real mount, unmount or eject invocation.
  for (const auto& [fixture, expected] : std::vector<std::pair<const char*, const char*>>{
      {"exit 0", ""}, {"printf 'Permission denied' >&2; exit 1", "Permission denied"},
      {"printf 'target is busy' >&2; exit 1", "target is busy"}}) {
    auto* process = g_subprocess_new(
        GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE),
        nullptr, "/bin/sh", "-c", fixture, nullptr);
    assert(process != nullptr);
    bool done = false;
    filemanager::PlacesModelTestAccess::wait(process, [&](std::string message) {
      assert(message == expected);
      done = true;
    });
    assert(!done); // The caller's guard survives until the helper is reaped.
    g_object_unref(process);
    while (!done)
      g_main_context_iteration(nullptr, true);
  }
  // Forged or stale snapshots must be rejected before any helper is launched.
  filemanager::Place stale{.label = "Gone", .identity = "nonexistent-fixture", .path = "/mnt/missing"};
  stale.canUnmount = true;
  stale.unixBackend = UnixUnmountBackend::UserMount;
  rejected = false;
  filemanager::PlacesModel::operate(stale, filemanager::PlaceOperation::Unmount, nullptr, nullptr,
      [&](std::string message) { rejected = message.find("changed") != std::string::npos; });
  assert(rejected);
  auto* cancelled = g_cancellable_new();
  g_cancellable_cancel(cancelled);
  rejected = false;
  filemanager::PlacesModel::operate(stale, filemanager::PlaceOperation::Unmount, nullptr, cancelled,
      [&](std::string message) { rejected = message.find("cancelled") != std::string::npos; });
  assert(rejected);
  g_object_unref(cancelled);
  std::vector<filemanager::Place> mountedOrder{
      {.label = "Alpha disconnected", .identity = "unmounted", .mounted = false},
      {.label = "Zulu mounted", .identity = "mounted"}};
  filemanager::orderPlaces(mountedOrder, {});
  assert(mountedOrder.front().mounted);
  filemanager::orderPlaces(mountedOrder, {"unmounted", "mounted"});
  assert(!mountedOrder.front().mounted); // Explicit saved ordering still wins.
  // Confirm the valid symlink case separately, without the duplicate category.
  std::filesystem::remove(root / "config/user-dirs.dirs");
  {
    std::ofstream config(root / "config/user-dirs.dirs");
    config << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n";
  }
  g_reload_user_special_dirs_cache();
  const auto refreshed = model.discover();
  const auto symlink = std::ranges::find_if(refreshed, [](const auto& place) { return place.label == "Pictures"; });
  assert(symlink != refreshed.end() && symlink->path == root / "Pictures");
  // One Personal collection permits bookmarks between built-ins. Device and
  // Network ranks survive temporary removal; new resources remain alphabetic.
  std::vector<filemanager::Place> entries{
      {.label = "Documents", .identity = "documents", .personal = true},
      {.label = "Downloads", .identity = "downloads", .personal = true},
      {.label = "Pinned", .identity = "pinned", .personal = true},
      {.label = "Zulu", .identity = "zulu"},
      {.label = "Alpha", .identity = "alpha"},
      {.label = "Zoo share", .identity = "net-z", .network = true},
      {.label = "Archive", .identity = "net-a", .network = true},
      {.label = "Duplicate", .identity = "alpha"}
  };
  filemanager::orderPlaces(entries, {});
  assert(entries.size() == 7 && entries[3].identity == "alpha" && entries[5].identity == "net-a");
  auto order = filemanager::reorderedPlaces(entries, {}, "pinned", "downloads");
  filemanager::orderPlaces(entries, order);
  assert(entries[1].identity == "pinned" && entries[2].identity == "downloads");
  order = filemanager::reorderedPlaces(entries, order, "zulu", "alpha");
  order = filemanager::reorderedPlaces(entries, order, "net-z", "net-a");
  const auto unchanged = filemanager::reorderedPlaces(entries, order, "zulu", "downloads");
  assert(unchanged == order); // Cross-group moves cannot reclassify resources.
  std::erase_if(entries, [](const auto& p) { return p.identity == "zulu"; });
  entries.push_back({.label = "Beta", .identity = "beta"});
  entries.push_back({.label = "Able", .identity = "able"});
  filemanager::orderPlaces(entries, order);
  assert(entries[3].identity == "alpha" && entries[4].identity == "able" && entries[5].identity == "beta");
  entries.push_back({.label = "Renamed device", .identity = "zulu"});
  filemanager::orderPlaces(entries, order);
  assert(entries[3].identity == "zulu" && entries[4].identity == "alpha");
  assert(entries[7].identity == "net-z" && entries[8].identity == "net-a");
  order = filemanager::reorderedPlaces(entries, order, "documents", "end:Personal");
  filemanager::orderPlaces(entries, order);
  assert(entries[0].identity == "pinned" && entries[2].identity == "documents");
  std::filesystem::remove_all(root);
}
