#include "calendar/vdir_reader.h"
#include "tests/test_check.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

  using namespace std::chrono;

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::filesystem::path createUniqueTempDir(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / (std::string(prefix) + "_" + std::to_string(now) + "_" + std::to_string(++counter));
    std::filesystem::create_directories(path);
    return path;
  }

  constexpr std::string_view kSampleIcs1 = "BEGIN:VCALENDAR\r\n"
                                           "VERSION:2.0\r\n"
                                           "PRODID:-//Example//Test//EN\r\n"
                                           "X-WR-CALNAME:Personal Cal\r\n"
                                           "BEGIN:VEVENT\r\n"
                                           "UID:evt-1@example.com\r\n"
                                           "DTSTAMP:20260101T000000Z\r\n"
                                           "DTSTART:20260820T100000Z\r\n"
                                           "DTEND:20260820T110000Z\r\n"
                                           "SUMMARY:Meeting 1\r\n"
                                           "END:VEVENT\r\n"
                                           "END:VCALENDAR\r\n";

  constexpr std::string_view kSampleIcs2 = "BEGIN:VCALENDAR\r\n"
                                           "VERSION:2.0\r\n"
                                           "PRODID:-//Example//Test//EN\r\n"
                                           "BEGIN:VEVENT\r\n"
                                           "UID:evt-2@example.com\r\n"
                                           "DTSTAMP:20260101T000000Z\r\n"
                                           "DTSTART:20260821T140000Z\r\n"
                                           "DTEND:20260821T150000Z\r\n"
                                           "SUMMARY:Meeting 2\r\n"
                                           "END:VEVENT\r\n"
                                           "END:VCALENDAR\r\n";

  void testNestedDiscovery() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_nested");

    // Create structure:
    // tempDir/
    //   fastmail_cal/
    //     uuid-1/
    //       event1.ics (has X-WR-CALNAME: Personal Cal)
    //     uuid-2/
    //       displayname ("Work Events")
    //       color ("#336699")
    //       order ("1")
    //       event2.ics
    //     .git/
    //       junk.ics (should be ignored)
    //     temp.tmp/
    //       junk.ics (should be ignored)
    writeFile(tempDir / "fastmail_cal" / "uuid-1" / "event1.ics", kSampleIcs1);
    writeFile(tempDir / "fastmail_cal" / "uuid-2" / "displayname", "Work Events\n");
    writeFile(tempDir / "fastmail_cal" / "uuid-2" / "color", "#336699\n");
    writeFile(tempDir / "fastmail_cal" / "uuid-2" / "order", "1\n");
    writeFile(tempDir / "fastmail_cal" / "uuid-2" / "event2.ics", kSampleIcs2);
    writeFile(tempDir / "fastmail_cal" / ".git" / "junk.ics", kSampleIcs1);
    writeFile(tempDir / "fastmail_cal" / "temp.tmp" / "junk.ics", kSampleIcs1);

    auto collections = calendar::discoverVdirCollections(tempDir);

    // Test loading events from col2
    const auto now = system_clock::now();
    std::size_t budget = 1000;
    std::vector<CalendarEvent> events;
    if (collections.size() == 2) {
      events = calendar::loadVdirCollectionEvents(collections[1], now - hours{24 * 365}, now + hours{24 * 365}, budget);
    }

    std::filesystem::remove_all(tempDir);

    TEST_CHECK(collections.size() == 2);

    // uuid-1 has order 0 (default), uuid-2 has order 1
    const auto& col1 = collections[0];
    const auto& col2 = collections[1];

    TEST_CHECK(col1.id == "fastmail_cal/uuid-1");
    TEST_CHECK(col1.name == "Personal Cal");
    TEST_CHECK(col1.colorHex.empty());

    TEST_CHECK(col2.id == "fastmail_cal/uuid-2");
    TEST_CHECK(col2.name == "Work Events");
    TEST_CHECK(col2.colorHex == "#336699");
    TEST_CHECK(col2.order == 1);

    TEST_CHECK(events.size() == 1);
    TEST_CHECK(budget == 999);
    TEST_CHECK(events[0].id == "evt-2@example.com");
    TEST_CHECK(events[0].title == "Meeting 2");
    TEST_CHECK(events[0].calendarName == "Work Events");
    TEST_CHECK(events[0].colorHex == "#336699");
  }

  void testDirectCollectionDiscovery() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_direct");

    writeFile(tempDir / "displayname", "Single Calendar\n");
    writeFile(tempDir / "color", "#FF5500\n");
    writeFile(tempDir / "event.ics", kSampleIcs1);

    auto collections = calendar::discoverVdirCollections(tempDir);

    std::filesystem::remove_all(tempDir);

    TEST_CHECK(collections.size() == 1);
    TEST_CHECK(collections[0].name == "Single Calendar");
    TEST_CHECK(collections[0].colorHex == "#FF5500");
    TEST_CHECK(collections[0].path == tempDir);
  }

  void testTrailingSlashRootDiscovery() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_slash");
    writeFile(tempDir / "subcal" / "event.ics", kSampleIcs1);

    const std::filesystem::path rootWithSlash = tempDir.string() + "/";
    auto collections = calendar::discoverVdirCollections(rootWithSlash);

    std::filesystem::remove_all(tempDir);

    TEST_CHECK(collections.size() == 1);
    TEST_CHECK(!collections[0].id.empty());
    TEST_CHECK(collections[0].id == "subcal");
  }

  // A bounded RRULE keeps the parser from fast-forwarding to the window start, so each file's expansion
  // walks day by day from 2000 up to the window end: roughly ten thousand recurrence-work units for the
  // ~730 events that land inside the window. Sixteen such files cost more than the parser's 100k
  // per-parse budget, so a budget shared across the collection would silently truncate the later files.
  std::string recurringIcs(int index) {
    return std::format(
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:recur-{}@example.com\r\n"
        "DTSTAMP:20000101T000000Z\r\n"
        "DTSTART:20000101T100000Z\r\n"
        "DTEND:20000101T110000Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=100000\r\n"
        "SUMMARY:Recurring {}\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n",
        index, index
    );
  }

  void testPerFileRecurrenceBudgetIsolation() {
    constexpr int kFileCount = 16;
    const auto oneDir = createUniqueTempDir("noctalia_vdir_test_budget_one");
    const auto manyDir = createUniqueTempDir("noctalia_vdir_test_budget_many");
    writeFile(oneDir / "cal" / "0.ics", recurringIcs(0));
    for (int i = 0; i < kFileCount; ++i) {
      writeFile(manyDir / "cal" / std::format("{}.ics", i), recurringIcs(i));
    }

    auto oneCollections = calendar::discoverVdirCollections(oneDir);
    auto manyCollections = calendar::discoverVdirCollections(manyDir);

    const bool discovered = oneCollections.size() == 1 && manyCollections.size() == 1;
    std::size_t oneEvents = 0;
    std::size_t manyEvents = 0;
    if (discovered) {
      // One window for both loads so the expansions are bit-for-bit comparable.
      const auto now = system_clock::now();
      const auto windowStart = now - hours{24 * 365};
      const auto windowEnd = now + hours{24 * 365};

      std::size_t oneBudget = std::numeric_limits<std::size_t>::max();
      oneEvents = calendar::loadVdirCollectionEvents(oneCollections[0], windowStart, windowEnd, oneBudget).size();
      std::size_t manyBudget = std::numeric_limits<std::size_t>::max();
      manyEvents = calendar::loadVdirCollectionEvents(manyCollections[0], windowStart, windowEnd, manyBudget).size();
    }

    std::filesystem::remove_all(oneDir);
    std::filesystem::remove_all(manyDir);

    TEST_CHECK(discovered);
    TEST_CHECK(oneEvents > 0);
    // Every file expands fully; the recurrence budget is not shared
    TEST_CHECK(manyEvents == oneEvents * kFileCount);
  }

  void testEventBudgetStopsRead() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_cap");
    writeFile(tempDir / "cal" / "0.ics", recurringIcs(0));
    writeFile(tempDir / "cal" / "1.ics", recurringIcs(1));

    auto collections = calendar::discoverVdirCollections(tempDir);
    std::size_t budget = 10;
    std::vector<CalendarEvent> events;
    if (collections.size() == 1) {
      const auto now = system_clock::now();
      events = calendar::loadVdirCollectionEvents(collections[0], now - hours{24 * 365}, now + hours{24 * 365}, budget);
    }

    std::filesystem::remove_all(tempDir);

    TEST_CHECK(collections.size() == 1);
    TEST_CHECK(events.size() == 10);
    TEST_CHECK(budget == 0);
  }

} // namespace

int main() {
  testNestedDiscovery();
  testDirectCollectionDiscovery();
  testTrailingSlashRootDiscovery();
  testPerFileRecurrenceBudgetIsolation();
  testEventBudgetStopsRead();
  return 0;
}
