#pragma once

#include "calendar/calendar_types.h"
#include "calendar/ical_parser.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

namespace calendar {

  struct VdirCollection {
    std::string id;             // Relative path or directory name identifier, e.g. "fastmail_cal/05059f01-..."
    std::string name;           // Display name (from displayname file, X-WR-CALNAME, or fallback)
    std::string colorHex;       // Hex color (e.g. "#4285F4" from color file)
    int order = 0;              // Order value from 'order' file if present
    std::filesystem::path path; // Full filesystem path to the directory containing *.ics files

    bool operator==(const VdirCollection&) const = default;
  };

  // Resolves the default XDG calendar path: $XDG_DATA_HOME/calendars or ~/.local/share/calendars.
  [[nodiscard]] std::filesystem::path defaultVdirPath();

  // Recursively discovers leaf directories containing *.ics files starting from rootPath up to maxDepth.
  [[nodiscard]] std::vector<VdirCollection>
  discoverVdirCollections(const std::filesystem::path& rootPath, int maxDepth = 5);

  // Reads all *.ics files in the collection and parses them within the given time window.
  // remainingEvents bounds how many events the caller is still willing to accept; it is decremented
  // as events are produced and reaching zero stops the read and logs the truncation.
  [[nodiscard]] std::vector<CalendarEvent> loadVdirCollectionEvents(
      const VdirCollection& collection, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd, std::size_t& remainingEvents, std::stop_token stopToken = {}
  );

} // namespace calendar
