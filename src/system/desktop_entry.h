#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DesktopAction {
  std::string id;
  std::string name;
  std::string exec;
};

struct DesktopEntry {
  std::string id;
  std::string path;
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

  // Pre-lowercased for matching
  std::string nameLower;
  std::string genericNameLower;
  std::string keywordsLower;
  std::string categoriesLower;
  std::string startupWmClassLower;
  std::string idLower;
  std::string execLower;

  // Desktop file actions (e.g. "New Window", "New Private Window")
  std::vector<DesktopAction> actions;
};

std::vector<DesktopEntry> scanDesktopEntries();

const std::vector<DesktopEntry>& desktopEntries();

// Shared snapshot of the current entry list, safe to call from non-main
// threads (e.g. plugin script workers). Does not trigger a refresh —
// freshness is owned by the main thread's reload path.
std::shared_ptr<const std::vector<DesktopEntry>> desktopEntriesSnapshot();

std::uint64_t desktopEntriesVersion();
int desktopEntryWatchFd() noexcept;
void checkDesktopEntryReload();

// Cheaply re-stat the resolved XDG application source directories and mark the
// cache dirty only if they changed. Catches Nix profile-generation symlink
// swaps that inotify cannot see (the watched store path is immutable).
void refreshDesktopEntriesIfSourcesChanged();
