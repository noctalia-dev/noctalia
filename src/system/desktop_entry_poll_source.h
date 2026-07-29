#pragma once

#include "app/poll_source.h"
#include "system/desktop_entry.h"

class DesktopEntryPollSource final : public PollSource {
public:
  // Prime the cache on the main thread and refresh it eagerly on change, so
  // worker-thread desktopEntriesSnapshot() readers see a populated, current
  // list even when no main-thread widget consumes desktopEntries().
  DesktopEntryPollSource() { desktopEntries(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override {
    if (desktopEntryWatchFd() >= 0 && (fds[startIdx].revents & POLLIN) != 0) {
      checkDesktopEntryReload();
      desktopEntries();
    }
  }

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override {
    if (desktopEntryWatchFd() >= 0) {
      fds.push_back({.fd = desktopEntryWatchFd(), .events = POLLIN, .revents = 0});
    }
  }
};
