#include "filemanager/places_model.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
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
    std::vector<std::string> labels;
    for (const auto& place : places)
      if (filemanager::placeGroup(place) == group)
        labels.push_back(place.label);
    assert(std::is_sorted(labels.begin(), labels.end(), [](const auto& a, const auto& b) {
      return g_utf8_collate(a.c_str(), b.c_str()) < 0;
    }));
  }
  bool rejected = false;
  filemanager::PlacesModel::operate({}, filemanager::PlaceOperation::Mount, nullptr, nullptr, [&](std::string message) {
    rejected = !message.empty();
  });
  assert(rejected);
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
