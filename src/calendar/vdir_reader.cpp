#include "calendar/vdir_reader.h"

#include "calendar/ical_parser.h"
#include "core/log.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace calendar {

  namespace {
    constexpr Logger kLog("vdir-reader");

    std::string readTrimmedFile(const std::filesystem::path& path) {
      std::error_code ec;
      if (!std::filesystem::is_regular_file(path, ec)) {
        return {};
      }
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return {};
      }
      std::ostringstream ss;
      ss << file.rdbuf();
      return StringUtils::trim(ss.str());
    }

    bool isValidColorHex(std::string_view hex) {
      if (hex.size() != 7 && hex.size() != 9) {
        return false;
      }
      if (hex.front() != '#') {
        return false;
      }
      return std::all_of(hex.begin() + 1, hex.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
      });
    }

    std::string extractCalNameFromIcs(std::string_view ics) {
      // Look for X-WR-CALNAME: or X-WR-CALNAME;...:
      constexpr std::string_view kCalNameKey = "X-WR-CALNAME";
      std::size_t pos = 0;
      while (pos < ics.size()) {
        std::size_t lineEnd = ics.find('\n', pos);
        if (lineEnd == std::string_view::npos) {
          lineEnd = ics.size();
        }
        std::string_view line = ics.substr(pos, lineEnd - pos);
        if (!line.empty() && line.back() == '\r') {
          line.remove_suffix(1);
        }

        if (line.size() >= kCalNameKey.size()) {
          std::string_view prefix = line.substr(0, kCalNameKey.size());
          if (StringUtils::equalsInsensitive(prefix, kCalNameKey)) {
            std::size_t colon = line.find(':', kCalNameKey.size());
            if (colon != std::string_view::npos) {
              std::string value = StringUtils::trim(line.substr(colon + 1));
              if (!value.empty()) {
                return value;
              }
            }
          }
        }

        if (lineEnd >= ics.size()) {
          break;
        }
        pos = lineEnd + 1;
      }
      return {};
    }

    bool isIcsFile(const std::filesystem::path& path) {
      const std::string filename = path.filename().string();
      if (filename.empty() || filename.front() == '.' || filename.ends_with(".tmp")) {
        return false;
      }
      return path.extension() == ".ics";
    }

    bool hasIcsFiles(const std::filesystem::path& dirPath) {
      std::error_code ec;
      if (!std::filesystem::is_directory(dirPath, ec)) {
        return false;
      }
      for (const auto& entry : std::filesystem::directory_iterator(
               dirPath, std::filesystem::directory_options::skip_permission_denied, ec
           )) {
        if (entry.is_regular_file(ec) && isIcsFile(entry.path())) {
          return true;
        }
      }
      return false;
    }

    VdirCollection buildCollection(const std::filesystem::path& rootPath, const std::filesystem::path& dirPath) {
      VdirCollection col;
      col.path = dirPath;

      std::error_code ec;
      if (rootPath == dirPath) {
        col.id = dirPath.filename().string();
      } else {
        col.id = std::filesystem::relative(dirPath, rootPath, ec).generic_string();
      }
      if (col.id.empty()) {
        col.id = dirPath.filename().string();
      }

      // Read displayname file
      std::string displayName = readTrimmedFile(dirPath / "displayname");
      if (displayName.empty()) {
        // Try reading X-WR-CALNAME from the first .ics file
        for (const auto& entry : std::filesystem::directory_iterator(
                 dirPath, std::filesystem::directory_options::skip_permission_denied, ec
             )) {
          if (entry.is_regular_file(ec) && isIcsFile(entry.path())) {
            std::ifstream icsFile(entry.path(), std::ios::binary);
            if (icsFile) {
              std::string header;
              header.resize(4096);
              icsFile.read(header.data(), static_cast<std::streamsize>(header.size()));
              header.resize(static_cast<std::size_t>(icsFile.gcount()));
              displayName = extractCalNameFromIcs(header);
              if (!displayName.empty()) {
                break;
              }
            }
          }
        }
      }

      if (displayName.empty()) {
        col.name = dirPath.filename().string();
      } else {
        col.name = std::move(displayName);
      }

      // Read color file
      std::string color = readTrimmedFile(dirPath / "color");
      if (isValidColorHex(color)) {
        col.colorHex = std::move(color);
      }

      // Read order file
      std::string orderStr = readTrimmedFile(dirPath / "order");
      if (!orderStr.empty()) {
        try {
          col.order = std::stoi(orderStr);
        } catch (...) {
          col.order = 0;
        }
      }

      return col;
    }
  } // namespace

  std::filesystem::path defaultVdirPath() {
    if (const char* xdgData = std::getenv("XDG_DATA_HOME"); xdgData != nullptr && *xdgData != '\0') {
      return std::filesystem::path(xdgData) / "calendars";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path(home) / ".local" / "share" / "calendars";
    }
    return std::filesystem::path(".local/share/calendars");
  }

  std::vector<VdirCollection> discoverVdirCollections(const std::filesystem::path& rootPath, int maxDepth) {
    std::vector<VdirCollection> collections;
    std::error_code ec;

    if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec)) {
      return collections;
    }

    // Direct collection check
    if (hasIcsFiles(rootPath)) {
      collections.push_back(buildCollection(rootPath, rootPath));
      return collections;
    }

    auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (auto it = std::filesystem::recursive_directory_iterator(rootPath, opts, ec);
         it != std::filesystem::recursive_directory_iterator();) {
      if (it.depth() > maxDepth) {
        it.pop();
        continue;
      }

      const auto& entry = *it;
      if (entry.is_directory(ec)) {
        const std::string dirname = entry.path().filename().string();
        if (!dirname.empty() && (dirname.front() == '.' || dirname.ends_with(".tmp"))) {
          it.disable_recursion_pending();
          it.increment(ec);
          continue;
        }

        if (hasIcsFiles(entry.path())) {
          collections.push_back(buildCollection(rootPath, entry.path()));
          it.disable_recursion_pending();
        }
      }

      it.increment(ec);
    }

    std::ranges::sort(collections, [](const VdirCollection& a, const VdirCollection& b) {
      if (a.order != b.order) {
        return a.order < b.order;
      }
      return StringUtils::naturalCaseInsensitiveCompare(a.id, b.id) < 0;
    });

    return collections;
  }

  std::vector<CalendarEvent> loadVdirCollectionEvents(
      const VdirCollection& collection, std::chrono::system_clock::time_point windowStart,
      std::chrono::system_clock::time_point windowEnd, ICalParseControl& control
  ) {
    std::vector<CalendarEvent> events;
    std::error_code ec;

    if (!std::filesystem::is_directory(collection.path, ec)) {
      return events;
    }

    for (const auto& entry : std::filesystem::directory_iterator(
             collection.path, std::filesystem::directory_options::skip_permission_denied, ec
         )) {
      if (!entry.is_regular_file(ec) || !isIcsFile(entry.path())) {
        continue;
      }

      std::ifstream file(entry.path(), std::ios::binary);
      if (!file) {
        continue;
      }
      std::ostringstream ss;
      ss << file.rdbuf();
      const std::string content = ss.str();
      if (content.empty()) {
        continue;
      }

      auto result = parseICalEvents(content, windowStart, windowEnd, control);
      for (auto& ev : result.events) {
        if (ev.calendarName.empty()) {
          ev.calendarName = collection.name;
        }
        if (ev.colorHex.empty() && !collection.colorHex.empty()) {
          ev.colorHex = collection.colorHex;
        }
        events.push_back(std::move(ev));
      }
    }

    return events;
  }

} // namespace calendar
