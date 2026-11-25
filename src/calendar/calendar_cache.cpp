#include "calendar/calendar_cache.h"

#include "calendar/calendar_reminders.h"
#include "calendar/event_link.h"
#include "util/file_utils.h"

#include <chrono>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace calendar::cache {
  namespace {
    const security::EncryptionContext kEncryptionContext{
        .purpose = "calendar-events-v1",
        .objectId = "events",
    };

    std::int64_t toUnix(std::chrono::system_clock::time_point tp) {
      return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    }

    std::chrono::system_clock::time_point fromUnix(std::int64_t seconds) {
      return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
    }
  } // namespace

  bool secureExisting(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path.parent_path(), ec)
        && !FileUtils::setPrivateDirectoryPermissions(path.parent_path(), ec)) {
      return false;
    }
    if (ec) {
      return false;
    }
    if (std::filesystem::exists(path, ec) && !FileUtils::setPrivateFilePermissions(path, ec)) {
      return false;
    }
    return !ec;
  }

  LegacyReadResult readLegacy(const std::filesystem::path& path, std::size_t maxBytes) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec) {
      return {
          .status = ec == std::errc::no_such_file_or_directory ? LegacyReadStatus::NotFound : LegacyReadStatus::IoError,
      };
    }
    if (!std::filesystem::exists(status)) {
      return {.status = LegacyReadStatus::NotFound};
    }
    if (!std::filesystem::is_regular_file(status)) {
      return {.status = LegacyReadStatus::IoError};
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
      return {.status = LegacyReadStatus::IoError};
    }
    if (size > maxBytes || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
      return {.status = LegacyReadStatus::Oversized};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
      return {.status = LegacyReadStatus::IoError};
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    if (!contents.empty()) {
      input.read(reinterpret_cast<char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
      if (!input.good()) {
        return {.status = LegacyReadStatus::IoError};
      }
    }
    if (input.peek() != std::char_traits<char>::eof()) {
      return {.status = LegacyReadStatus::IoError};
    }
    return {.status = LegacyReadStatus::Success, .contents = std::move(contents)};
  }

  security::EncryptedReadResult
  readEncrypted(const std::filesystem::path& path, const security::SecureKey& key, std::size_t maxPlaintextBytes) {
    return security::readEncryptedFile(path, key, kEncryptionContext, maxPlaintextBytes);
  }

  bool writeEncrypted(const std::filesystem::path& path, std::string_view content, const security::SecureKey& key) {
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    return security::writeEncryptedFile(path, bytes, key, kEncryptionContext);
  }

  std::string serializeEvents(const std::map<std::string, std::vector<CalendarEvent>>& eventsByAccount) {
    nlohmann::json events = nlohmann::json::array();
    for (const auto& [accountId, accountEvents] : eventsByAccount) {
      for (const CalendarEvent& event : accountEvents) {
        events.push_back({
            {"account", accountId},
            {"id", event.id},
            {"title", event.title},
            {"calendar", event.calendarName},
            {"color", event.colorHex},
            {"source", event.sourceId},
            {"location", event.location},
            {"url", event.url},
            {"web_url", event.webUrl},
            {"start", toUnix(event.start)},
            {"end", toUnix(event.end)},
            {"all_day", event.allDay},
            {"declined", event.declined},
        });
        if (event.reminderLeadSeconds.has_value()) {
          events.back()["reminders"] = *event.reminderLeadSeconds;
        }
      }
    }
    return nlohmann::json{{"events", std::move(events)}}.dump();
  }

  std::map<std::string, std::vector<CalendarEvent>> parseEvents(std::string_view json) {
    const auto j = nlohmann::json::parse(json);
    std::map<std::string, std::vector<CalendarEvent>> parsed;
    for (const auto& item : j.at("events")) {
      CalendarEvent event;
      event.id = item.value("id", std::string{});
      event.title = item.value("title", std::string{});
      event.calendarName = item.value("calendar", std::string{});
      event.colorHex = item.value("color", std::string{});
      event.sourceId = item.value("source", std::string{});
      event.location = item.value("location", std::string{});
      event.url = calendar::resolveEventLink(event.location, {}, item.value("url", std::string{}));
      event.webUrl = calendar::resolveEventLink({}, {}, item.value("web_url", std::string{}));
      event.start = fromUnix(item.value("start", std::int64_t{0}));
      event.end = fromUnix(item.value("end", std::int64_t{0}));
      event.allDay = item.value("all_day", false);
      event.declined = item.value("declined", false);
      // A missing key means the source said nothing; an empty array means "explicitly no reminder".
      if (const auto reminders = item.find("reminders"); reminders != item.end() && reminders->is_array()) {
        auto leads = reminders->get<std::vector<std::int32_t>>();
        // Normalize on read so a hand-edited or corrupted cache cannot inflate the fired set.
        calendar::normalizeReminderLeads(leads);
        event.reminderLeadSeconds = std::move(leads);
      }
      const std::string account = item.value("account", std::string{});
      parsed[account].push_back(std::move(event));
    }
    return parsed;
  }
} // namespace calendar::cache
