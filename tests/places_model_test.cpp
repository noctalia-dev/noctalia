#include "filemanager/places_model.h"
#include "test_isolation.h"

#include <algorithm>
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
    static void setOperationActive(PlacesModel& model, bool active) {
      PlacesModel::setOperationActiveForTesting(model, active);
    }
  };
} // namespace filemanager

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-places-test"});
    TEST_CHECK(filemanager::internalKioFuseMount("/run/user/1000/kio-fuse-YFChrh", "fuse.kio-fuse", "kio-fuse"));
    TEST_CHECK(filemanager::internalKioFuseMount("/run/user/1000/kio-fuse-YFChrh", "fuse", "kio-fuse"));
    TEST_CHECK(
        !filemanager::internalKioFuseMount("/run/user/1000/kio-fuse-YFChrh", "fuse.rclone", "remote:CapsuleCorp")
    );
    TEST_CHECK(!filemanager::internalKioFuseMount("/run/user/1000/kio-fuse-YFChrh", "fuse", "remote:CapsuleCorp"));
    TEST_CHECK(!filemanager::internalKioFuseMount("/mnt/kio-fuse-YFChrh", "fuse.kio-fuse", "kio-fuse"));
    TEST_CHECK(
        !filemanager::internalKioFuseMount("/run/user/1000/rclone-CapsuleCorp", "fuse.rclone", "remote:CapsuleCorp")
    );
    TEST_CHECK(!filemanager::internalKioFuseMount("/run/user/name/kio-fuse-YFChrh"));
    {
      filemanager::Place device{
          .label = "Home",
          .details = "rw,relatime,ssd,compress=zstd:3,subvolid=256",
          .filesystemType = "btrfs",
          .mountSource = "/dev/test",
          .path = "/home"
      };
      constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
      const auto tooltip = filemanager::placeTooltip(device, {}, 68 * gib, 176 * gib);
      TEST_CHECK(tooltip.starts_with("Home\n/home · Btrfs\n"));
      TEST_CHECK(tooltip.find("GiB free of ") != std::string::npos);
      TEST_CHECK(tooltip.find("176") != std::string::npos);
      TEST_CHECK(tooltip.find("relatime") == std::string::npos && tooltip.find("/dev/test") == std::string::npos);
      TEST_CHECK(filemanager::placeTooltip(device) == "Home\n/home · Btrfs");
      device.mounted = false;
      TEST_CHECK(filemanager::placeTooltip(device, {}, 68 * gib, 176 * gib) == "Home\nNot mounted");
      device = {.label = "Unknown"};
      TEST_CHECK(filemanager::placeTooltip(device) == "Unknown");
      TEST_CHECK(filemanager::placeTooltip(device, "ext4").ends_with("\next4"));
      device = {
          .label = "Share",
          .filesystemType = "nfs4",
          .mountSource = "server:/share",
          .path = "/mnt/share",
          .network = true
      };
      TEST_CHECK(filemanager::placeTooltip(device) == "Share\n/mnt/share · nfs4 · server:/share");
      device.network = false;
      device.filesystemType = "fuse.rclone";
      device.mountSource = "remote:folder";
      TEST_CHECK(filemanager::placeTooltip(device).ends_with(" · fuse.rclone · remote:folder"));
      TEST_CHECK(filemanager::placeTooltip(device, {}, 0, gib).find("free of") != std::string::npos);
    }
    const auto& root = env.home();
    const auto& configDir = env.config();
    std::filesystem::create_directories(root / "actual-pictures");
    std::filesystem::create_directory_symlink(root / "actual-pictures", root / "Pictures");
    std::filesystem::create_directory_symlink(root / "missing", root / "Videos");
    {
      std::ofstream config(configDir / "user-dirs.dirs");
      config
          << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
          << "XDG_VIDEOS_DIR=\"$HOME/Videos\"\n"
          << "XDG_DOCUMENTS_DIR=\"$HOME/actual-pictures\"\n"
          << "XDG_MUSIC_DIR=\"$HOME/\"\n";
    }
    g_reload_user_special_dirs_cache();
    filemanager::PlacesModel model(false);
    bool discoveryChanged = false;
    model.setChangedCallback([&] { discoveryChanged = true; });
    GVolumeMonitor* monitor = g_volume_monitor_get();
    // Volume monitor signals carry a device argument in addition to the sender.
    // Unix mount-monitor signals do not; both must reach the right model owner.
    g_signal_emit_by_name(monitor, "volume-changed", nullptr);
    while (g_main_context_iteration(nullptr, false)) {
    }
    TEST_CHECK(discoveryChanged);
    g_object_unref(monitor);
    std::vector<std::filesystem::path> liveKioMounts;
    GList* liveMountEntries = g_unix_mount_entries_get(nullptr);
    for (auto* it = liveMountEntries; it != nullptr; it = it->next) {
      auto* entry = static_cast<GUnixMountEntry*>(it->data);
      const char* mountPath = g_unix_mount_entry_get_mount_path(entry);
      const char* source = g_unix_mount_entry_get_device_path(entry);
      const char* type = g_unix_mount_entry_get_fs_type(entry);
      if (mountPath != nullptr
          && filemanager::internalKioFuseMount(
              mountPath, type == nullptr ? std::string_view{} : type, source == nullptr ? std::string_view{} : source
          ))
        liveKioMounts.emplace_back(mountPath);
    }
    g_list_free_full(liveMountEntries, reinterpret_cast<GDestroyNotify>(g_unix_mount_entry_free));
    const auto places = model.discover();
    TEST_CHECK(std::ranges::none_of(places, [&](const auto& place) {
      return std::ranges::any_of(liveKioMounts, [&](const auto& path) {
        return place.path.lexically_normal() == path.lexically_normal();
      });
    }));
    TEST_CHECK(std::ranges::none_of(places, [](const auto& place) {
      return filemanager::internalKioFuseMount(place.path, place.filesystemType, place.mountSource);
    }));
    const auto pictures = std::ranges::find_if(places, [](const auto& place) {
      return place.personal && (place.label == "Pictures" || place.label == "Documents");
    });
    TEST_CHECK(pictures != places.end());
    TEST_CHECK(!pictures->iconNames.empty() && pictures->iconNames.back() == "folder");
    const auto homePlace = std::ranges::find_if(places, [](const auto& place) { return place.label == "Home"; });
    TEST_CHECK(homePlace != places.end() && homePlace->iconNames.front() == "user-home");
    // Documents and Pictures point to the same target and must appear only once.
    TEST_CHECK(
        std::ranges::count_if(
            places,
            [](const auto& place) {
              return place.personal && (place.label == "Pictures" || place.label == "Documents");
            }
        )
        == 1
    );
    TEST_CHECK(std::ranges::none_of(places, [](const auto& place) {
      return place.personal && (place.label == "Videos" || place.label == "Music");
    }));
    TEST_CHECK(!std::filesystem::exists(root / "missing"));
    for (const auto group : {"Devices", "Network"}) {
      std::vector<std::pair<bool, std::string>> labels;
      for (const auto& place : places)
        if (filemanager::placeGroup(place) == group)
          labels.emplace_back(place.mounted, place.label);
      TEST_CHECK(std::is_sorted(labels.begin(), labels.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first : g_utf8_collate(a.second.c_str(), b.second.c_str()) < 0;
      }));
    }
    bool rejected = false;
    filemanager::PlacesModel::operate(
        {}, filemanager::PlaceOperation::Mount, nullptr, nullptr,
        [&](std::string message) { rejected = !message.empty(); }
    );
    TEST_CHECK(rejected);
    using filemanager::UnixUnmountBackend;
    const auto backend = [](std::string_view path, std::string_view type, std::string_view options,
                            std::string_view configured = "", bool fuse = true, bool umount = true) {
      return filemanager::unixUnmountBackend(path, type, options, configured, 1000, fuse, umount);
    };
    TEST_CHECK(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1000,group_id=1000") == UnixUnmountBackend::Fuse);
    TEST_CHECK(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1001") == UnixUnmountBackend::None);
    TEST_CHECK(backend("/mnt/remote", "fuse.rclone", "rw,user_id=10000") == UnixUnmountBackend::None);
    TEST_CHECK(backend("/mnt/remote", "fuse.rclone", "rw,user_id=1000", "", false) == UnixUnmountBackend::None);
    TEST_CHECK(backend("/mnt/share", "nfs4", "rw", "rw,users,_netdev") == UnixUnmountBackend::UserMount);
    TEST_CHECK(backend("/mnt/disk", "ext4", "rw", "user,noauto") == UnixUnmountBackend::UserMount);
    TEST_CHECK(backend("/mnt/share", "nfs4", "rw", "rw,_netdev") == UnixUnmountBackend::None);
    TEST_CHECK(backend("/mnt/share", "nfs4", "rw", "x-users") == UnixUnmountBackend::None);
    TEST_CHECK(backend("/mnt/share", "nfs4", "rw", "users", true, false) == UnixUnmountBackend::None);
    for (const auto path : {"/", "/home", "/boot", "/var/lib/data", "/run/service", "relative"})
      TEST_CHECK(backend(path, "ext4", "rw", "users") == UnixUnmountBackend::None);
    TEST_CHECK(backend("/run/media/user/disk", "ext4", "rw", "users") == UnixUnmountBackend::UserMount);
    // Production dispatch keeps unusual mount paths in one inert argument.
    const std::string unusualPath = "/mnt/a share;$(false)";
    TEST_CHECK(
        (filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::Fuse, unusualPath)
         == std::vector<std::string>{"/usr/bin/fusermount3", "-u", "--", unusualPath})
    );
    TEST_CHECK(
        (filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::UserMount, unusualPath)
         == std::vector<std::string>{"/usr/bin/umount", "--", unusualPath})
    );
    TEST_CHECK(filemanager::PlacesModelTestAccess::arguments(UnixUnmountBackend::None, unusualPath).empty());
    // Controlled helpers only: no real mount, unmount or eject invocation.
    for (const auto& [fixture, expected] : std::vector<std::pair<const char*, const char*>>{
             {"exit 0", ""},
             {"printf 'Permission denied' >&2; exit 1", "Permission to unmount this filesystem was denied."},
             {"printf 'target is busy' >&2; exit 1", "This device is busy. Close open files and try again."}
         }) {
      auto* process = g_subprocess_new(
          GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE), nullptr, "/bin/sh",
          "-c", fixture, nullptr
      );
      TEST_CHECK(process != nullptr);
      bool done = false;
      std::string actualMessage;
      filemanager::PlacesModelTestAccess::wait(process, [&](std::string message) {
        actualMessage = std::move(message);
        done = true;
      });
      TEST_CHECK(!done); // The caller's guard survives until the helper is reaped.
      g_object_unref(process);
      while (!done)
        g_main_context_iteration(nullptr, true);
      TEST_CHECK(actualMessage == expected);
    }
    // Forged or stale snapshots must be rejected before any helper is launched.
    filemanager::Place stale{.label = "Gone", .identity = "nonexistent-fixture", .path = "/mnt/missing"};
    stale.canUnmount = true;
    stale.unixBackend = UnixUnmountBackend::UserMount;
    rejected = false;
    filemanager::PlacesModel::operate(
        stale, filemanager::PlaceOperation::Unmount, nullptr, nullptr,
        [&](std::string message) { rejected = message.find("changed") != std::string::npos; }
    );
    TEST_CHECK(rejected);
    auto* cancelled = g_cancellable_new();
    g_cancellable_cancel(cancelled);
    rejected = false;
    filemanager::PlacesModel::operate(
        stale, filemanager::PlaceOperation::Unmount, nullptr, cancelled,
        [&](std::string message) { rejected = message.find("cancelled") != std::string::npos; }
    );
    TEST_CHECK(rejected);
    g_object_unref(cancelled);
    std::vector<filemanager::Place> mountedOrder{
        {.label = "Alpha disconnected", .identity = "unmounted", .mounted = false},
        {.label = "Zulu mounted", .identity = "mounted"}
    };
    filemanager::orderPlaces(mountedOrder, {});
    TEST_CHECK(mountedOrder.front().mounted);
    filemanager::orderPlaces(mountedOrder, {"unmounted", "mounted"});
    TEST_CHECK(!mountedOrder.front().mounted); // Explicit saved ordering still wins.
    // Confirm the valid symlink case separately, without the duplicate category.
    std::filesystem::remove(configDir / "user-dirs.dirs");
    {
      std::ofstream config(configDir / "user-dirs.dirs");
      config << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n";
    }
    g_reload_user_special_dirs_cache();
    const auto refreshed = model.discover();
    const auto symlink = std::ranges::find_if(refreshed, [](const auto& place) { return place.label == "Pictures"; });
    TEST_CHECK(symlink != refreshed.end() && symlink->path == root / "Pictures");
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
    TEST_CHECK(entries.size() == 7 && entries[3].identity == "alpha" && entries[5].identity == "net-a");
    auto order = filemanager::reorderedPlaces(entries, {}, "pinned", "downloads");
    filemanager::orderPlaces(entries, order);
    TEST_CHECK(entries[1].identity == "pinned" && entries[2].identity == "downloads");
    order = filemanager::reorderedPlaces(entries, order, "zulu", "alpha");
    order = filemanager::reorderedPlaces(entries, order, "net-z", "net-a");
    const auto unchanged = filemanager::reorderedPlaces(entries, order, "zulu", "downloads");
    TEST_CHECK(unchanged == order); // Cross-group moves cannot reclassify resources.
    std::erase_if(entries, [](const auto& p) { return p.identity == "zulu"; });
    entries.push_back({.label = "Beta", .identity = "beta"});
    entries.push_back({.label = "Able", .identity = "able"});
    filemanager::orderPlaces(entries, order);
    TEST_CHECK(entries[3].identity == "alpha" && entries[4].identity == "able" && entries[5].identity == "beta");
    entries.push_back({.label = "Renamed device", .identity = "zulu"});
    filemanager::orderPlaces(entries, order);
    TEST_CHECK(entries[3].identity == "zulu" && entries[4].identity == "alpha");
    TEST_CHECK(entries[7].identity == "net-z" && entries[8].identity == "net-a");
    order = filemanager::reorderedPlaces(entries, order, "documents", "end:Personal");
    filemanager::orderPlaces(entries, order);
    TEST_CHECK(entries[0].identity == "pinned" && entries[2].identity == "documents");

    // F-10: Scoped RAII operation guard does not block independent PlacesModel instances
    // and never gets stuck when a completion callback throws or re-enters.
    {
      filemanager::PlacesModel modelA(false);
      filemanager::PlacesModel modelB(false);

      // Throwing inside a completion callback must still release the guard.
      bool threw = false;
      try {
        modelA.operateDevice({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [](std::string) {
          throw std::runtime_error("simulated callback error");
        });
      } catch (const std::runtime_error&) {
        threw = true;
      }
      TEST_CHECK(threw);

      // Subsequent operation on modelA must NOT be permanently stuck with "Wait for the current device operation".
      std::string afterThrow;
      modelA.operateDevice({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [&](std::string message) {
        afterThrow = std::move(message);
      });
      TEST_CHECK(afterThrow.find("Wait for the current device operation") == std::string::npos);
      TEST_CHECK(afterThrow.find("no longer supports") != std::string::npos);

      // Holding an active operation on modelA rejects overlapping calls on modelA
      // without blocking independent modelB.
      filemanager::PlacesModelTestAccess::setOperationActive(modelA, true);
      std::string overlapMsg;
      modelA.operateDevice({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [&](std::string msg) {
        overlapMsg = std::move(msg);
      });
      TEST_CHECK(overlapMsg == "Wait for the current device operation to finish.");

      std::string independentMsg;
      modelB.operateDevice({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [&](std::string msg) {
        independentMsg = std::move(msg);
      });
      TEST_CHECK(independentMsg != "Wait for the current device operation to finish.");
      TEST_CHECK(independentMsg.find("no longer supports") != std::string::npos);
      filemanager::PlacesModelTestAccess::setOperationActive(modelA, false);
    }
  });
}
