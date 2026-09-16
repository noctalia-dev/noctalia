#include "calendar/vdir_reader.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <string>
#include <vector>

namespace {

  using namespace std::chrono;

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "vdir_reader_test: {}", message);
    }
    return condition;
  }

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

  bool testNestedDiscovery() {
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

    bool ok = true;
    ok &= expect(collections.size() == 2, "Expected 2 discovered collections");

    if (collections.size() == 2) {
      // uuid-1 has order 0 (default), uuid-2 has order 1
      const auto& col1 = collections[0];
      const auto& col2 = collections[1];

      ok &= expect(col1.id == "fastmail_cal/uuid-1", "col1 id matches relative path");
      ok &= expect(col1.name == "Personal Cal", "col1 extracted X-WR-CALNAME");
      ok &= expect(col1.colorHex.empty(), "col1 has empty color");

      ok &= expect(col2.id == "fastmail_cal/uuid-2", "col2 id matches relative path");
      ok &= expect(col2.name == "Work Events", "col2 read displayname file");
      ok &= expect(col2.colorHex == "#336699", "col2 read color file");
      ok &= expect(col2.order == 1, "col2 read order file");

      // Test loading events from col2
      const auto now = system_clock::now();
      std::size_t budget = 1000;
      auto events = calendar::loadVdirCollectionEvents(col2, now - hours{24 * 365}, now + hours{24 * 365}, budget);
      ok &= expect(events.size() == 1, "col2 loaded 1 event");
      ok &= expect(budget == 999, "col2 consumed one event from the budget");
      if (!events.empty()) {
        ok &= expect(events[0].id == "evt-2@example.com", "event id matches");
        ok &= expect(events[0].title == "Meeting 2", "event title matches");
        ok &= expect(events[0].calendarName == "Work Events", "event calendarName matches");
        ok &= expect(events[0].colorHex == "#336699", "event colorHex matches");
      }
    }

    std::filesystem::remove_all(tempDir);
    return ok;
  }

  bool testDirectCollectionDiscovery() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_direct");

    writeFile(tempDir / "displayname", "Single Calendar\n");
    writeFile(tempDir / "color", "#FF5500\n");
    writeFile(tempDir / "event.ics", kSampleIcs1);

    auto collections = calendar::discoverVdirCollections(tempDir);

    bool ok = true;
    ok &= expect(collections.size() == 1, "Expected 1 direct collection");
    if (!collections.empty()) {
      ok &= expect(collections[0].name == "Single Calendar", "Read displayname for direct collection");
      ok &= expect(collections[0].colorHex == "#FF5500", "Read color for direct collection");
      ok &= expect(collections[0].path == tempDir, "Path matches tempDir");
    }

    std::filesystem::remove_all(tempDir);
    return ok;
  }

  bool testTrailingSlashRootDiscovery() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_slash");
    writeFile(tempDir / "subcal" / "event.ics", kSampleIcs1);

    const std::filesystem::path rootWithSlash = tempDir.string() + "/";
    auto collections = calendar::discoverVdirCollections(rootWithSlash);

    bool ok = true;
    ok &= expect(collections.size() == 1, "Expected 1 collection with trailing slash root");
    if (!collections.empty()) {
      ok &= expect(!collections[0].id.empty(), "Collection ID must not be empty with trailing slash root");
      ok &= expect(collections[0].id == "subcal", "Collection ID matches subfolder name");
    }

    std::filesystem::remove_all(tempDir);
    return ok;
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

  bool testPerFileRecurrenceBudgetIsolation() {
    constexpr int kFileCount = 16;
    const auto oneDir = createUniqueTempDir("noctalia_vdir_test_budget_one");
    const auto manyDir = createUniqueTempDir("noctalia_vdir_test_budget_many");
    writeFile(oneDir / "cal" / "0.ics", recurringIcs(0));
    for (int i = 0; i < kFileCount; ++i) {
      writeFile(manyDir / "cal" / std::format("{}.ics", i), recurringIcs(i));
    }

    auto oneCollections = calendar::discoverVdirCollections(oneDir);
    auto manyCollections = calendar::discoverVdirCollections(manyDir);

    bool ok = true;
    ok &= expect(oneCollections.size() == 1 && manyCollections.size() == 1, "Discovered both budget collections");
    if (oneCollections.size() == 1 && manyCollections.size() == 1) {
      // One window for both loads so the expansions are bit-for-bit comparable.
      const auto now = system_clock::now();
      const auto windowStart = now - hours{24 * 365};
      const auto windowEnd = now + hours{24 * 365};

      std::size_t oneBudget = std::numeric_limits<std::size_t>::max();
      const auto oneEvents =
          calendar::loadVdirCollectionEvents(oneCollections[0], windowStart, windowEnd, oneBudget).size();
      std::size_t manyBudget = std::numeric_limits<std::size_t>::max();
      const auto manyEvents =
          calendar::loadVdirCollectionEvents(manyCollections[0], windowStart, windowEnd, manyBudget).size();

      ok &= expect(oneEvents > 0, "Recurring fixture expands to at least one event");
      ok &=
          expect(manyEvents == oneEvents * kFileCount, "Every file expands fully; the recurrence budget is not shared");
    }

    std::filesystem::remove_all(oneDir);
    std::filesystem::remove_all(manyDir);
    return ok;
  }

  bool testEventBudgetStopsRead() {
    const auto tempDir = createUniqueTempDir("noctalia_vdir_test_cap");
    writeFile(tempDir / "cal" / "0.ics", recurringIcs(0));
    writeFile(tempDir / "cal" / "1.ics", recurringIcs(1));

    auto collections = calendar::discoverVdirCollections(tempDir);
    bool ok = true;
    ok &= expect(collections.size() == 1, "Discovered collection for event cap test");
    if (collections.size() == 1) {
      const auto now = system_clock::now();
      std::size_t budget = 10;
      const auto events =
          calendar::loadVdirCollectionEvents(collections[0], now - hours{24 * 365}, now + hours{24 * 365}, budget);
      ok &= expect(events.size() == 10, "Event budget caps the returned events");
      ok &= expect(budget == 0, "Event budget is fully consumed");
    }

    std::filesystem::remove_all(tempDir);
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok &= testNestedDiscovery();
  ok &= testDirectCollectionDiscovery();
  ok &= testTrailingSlashRootDiscovery();
  ok &= testPerFileRecurrenceBudgetIsolation();
  ok &= testEventBudgetStopsRead();

  if (ok) {
    std::println("vdir_reader_test passed");
    return 0;
  }
  return 1;
}
