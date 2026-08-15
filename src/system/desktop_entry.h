#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct DesktopAction {
  std::string id;
  std::string name;
  std::string exec;
  // Pre-lowercased for matching
  std::string nameLower;
  std::string execLower;
};

enum class DesktopEntryOrigin : std::uint8_t {
  Unknown,
  User,
  System,
  Flatpak,
  Snap,
  Nix,
  AppImage,
};

struct DesktopEntry {
  std::string id;
  std::string path;
  DesktopEntryOrigin origin = DesktopEntryOrigin::Unknown;
  std::string name;
  std::string genericName;
  std::string comment;
  std::string exec;
  std::string icon;
  std::string categories;
  std::string keywords;
  std::string startupWmClass;
  std::string workingDir;
  bool noDisplay = false;
  bool hidden = false;
  bool terminal = false;
  bool dbusActivatable = false;

  // Pre-lowercased for matching
  std::string nameLower;
  std::vector<std::string> localizedNamesLower;
  std::string genericNameLower;
  std::string keywordsLower;
  std::string categoriesLower;
  std::string startupWmClassLower;
  std::string idLower;
  std::string execLower;

  // Desktop file actions (e.g. "New Window", "New Private Window")
  std::vector<DesktopAction> actions;
};

std::vector<DesktopEntry> scanDesktopEntries(std::string_view language = {});

const std::vector<DesktopEntry>& desktopEntries();

// Shared snapshot of the current entry list, safe to call from non-main
// threads (e.g. plugin script workers). Does not trigger a refresh —
// freshness is owned by the main thread's reload path.
std::shared_ptr<const std::vector<DesktopEntry>> desktopEntriesSnapshot();

std::uint64_t desktopEntriesVersion();
void setDesktopEntryLanguage(std::string_view language);
int desktopEntryWatchFd() noexcept;
void checkDesktopEntryReload();

// Cheaply re-stat the resolved XDG application source directories and mark the
// cache dirty only if they changed. Catches Nix profile-generation symlink
// swaps that inotify cannot see (the watched store path is immutable).
void refreshDesktopEntriesIfSourcesChanged();
