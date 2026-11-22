#include "scripting/plugin_api.h"
#include "scripting/plugin_catalog.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace {

  // A catalog row only resolves to an older release when the tip is out of range and the
  // release is in range, so the fixtures are built from the supported range itself rather
  // than from literal levels that drift with every API bump.
  static_assert(scripting::kOldestSupportedPluginApiVersion > 1);
  static_assert(scripting::kOldestSupportedPluginApiVersion < scripting::kCurrentPluginApiVersion);

  constexpr std::uint32_t kTooNew = scripting::kCurrentPluginApiVersion + 1;
  constexpr std::uint32_t kNewest = scripting::kCurrentPluginApiVersion;
  constexpr std::uint32_t kOldest = scripting::kOldestSupportedPluginApiVersion;
  constexpr std::uint32_t kTooOld = scripting::kOldestSupportedPluginApiVersion - 1;

  constexpr std::string_view kHeadRev = "1111111111111111111111111111111111111111";
  constexpr std::string_view kNewestRev = "2222222222222222222222222222222222222222";
  constexpr std::string_view kOldestRev = "3333333333333333333333333333333333333333";

  std::chrono::system_clock::time_point unixSeconds(std::uint64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
  }

  std::string row(std::uint32_t pluginApiVersion, std::string_view releases, std::string_view version = "2.0.0") {
    return std::format(
        "[[plugin]]\n"
        "id = \"me/demo\"\n"
        "name = \"Demo\"\n"
        "version = \"{}\"\n"
        "plugin_api = {}\n"
        "{}",
        version, pluginApiVersion, releases
    );
  }

  std::string release(std::uint32_t pluginApiVersion, std::string_view version, std::string_view rev) {
    return std::format(
        "[[plugin.release]]\n"
        "plugin_api = {}\n"
        "version = \"{}\"\n"
        "rev = \"{}\"\n",
        pluginApiVersion, version, rev
    );
  }

} // namespace

int main() {
  {
    // A supported tip wins outright: no release is consulted and the export targets the
    // revision the catalog itself was read from.
    const auto entries = scripting::parseCatalogToml(row(kNewest, release(kOldest, "1.0.0", kOldestRev)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.compatible);
    TEST_CHECK(!e.heldBack);
    TEST_CHECK(e.resolvedVersion == "2.0.0");
    TEST_CHECK(e.resolvedRevision == kHeadRev);
    TEST_CHECK(e.resolvedPluginApiVersion == kNewest);
  }

  {
    // The whole point: a tip above the range falls back to the newest release below it.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", kNewestRev)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.compatible);
    TEST_CHECK(e.heldBack);
    TEST_CHECK(e.resolvedVersion == "1.9.0");
    TEST_CHECK(e.resolvedRevision == kNewestRev);
    TEST_CHECK(e.resolvedPluginApiVersion == kNewest);
    // The tip version and API level stay readable for the UI.
    TEST_CHECK(e.version == "2.0.0");
    TEST_CHECK(e.pluginApiVersion == kTooNew);
  }

  {
    // Highest supported release wins, and declaration order must not decide it.
    const std::string releases = release(kOldest, "1.0.0", kOldestRev) + release(kNewest, "1.9.0", kNewestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, releases), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.releases.size() == 2);
    TEST_CHECK(e.resolvedVersion == "1.9.0");
  }

  {
    // Nothing in range in either direction: the row stays uninstallable rather than
    // silently landing a version the host cannot run.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kTooOld, "0.1.0", kOldestRev)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(!e.compatible);
    TEST_CHECK(!e.heldBack);
    TEST_CHECK(e.resolvedVersion == "2.0.0");
  }

  {
    // A revision is interpolated into a git command line, so anything that is not a full
    // sha is dropped and the next release is used instead.
    const std::string releases = release(kNewest, "1.9.0", "not-a-sha") + release(kOldest, "1.0.0", kOldestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, releases), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.releases.size() == 1);
    TEST_CHECK(e.resolvedVersion == "1.0.0");
  }

  {
    // An uppercase sha is not what git prints, so it is malformed too.
    const std::string upper(40, 'A');
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", upper)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    TEST_CHECK(entries.front().releases.empty());
    TEST_CHECK(!entries.front().compatible);
  }

  {
    // A release at or above the tip can never be the answer, so it is a catalog error.
    const auto entries = scripting::parseCatalogToml(row(kNewest, release(kNewest, "1.9.0", kNewestRev)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    TEST_CHECK(entries.front().releases.empty());
  }

  {
    // A release without a version cannot be compared against the exported copy on disk.
    const std::string bad = std::format("[[plugin.release]]\nplugin_api = {}\nrev = \"{}\"\n", kNewest, kNewestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, bad), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    TEST_CHECK(entries.front().releases.empty());
  }
  {
    // A catalog row with a non-canonical version is dropped.
    const auto entries = scripting::parseCatalogToml(row(kNewest, "", "1.2.3-beta.1"), kHeadRev);
    TEST_CHECK(entries.empty());
  }

  {
    // An invalid release version drops the release but not its catalog row.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "01.9.0", kNewestRev)), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    TEST_CHECK(entries.front().releases.empty());
    TEST_CHECK(!entries.front().compatible);
  }

  {
    // A path source has no revisions to export, so its releases never apply.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", kNewestRev)), "");
    TEST_CHECK(entries.size() == 1);
    TEST_CHECK(!entries.front().compatible);
    TEST_CHECK(!entries.front().heldBack);
  }

  {
    // The store sorts on these two, so they must survive the parse as unix seconds.
    const std::string dated = row(kNewest, "") + "updated_at = 1750000000\nadded_at = 1700000000\n";
    const auto entries = scripting::parseCatalogToml(dated, kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.updatedAt == unixSeconds(1750000000));
    TEST_CHECK(e.addedAt == unixSeconds(1700000000));
  }

  {
    // Sources predating the date fields must still parse; the store falls back to a name
    // tiebreak when every entry lands on the epoch.
    const auto entries = scripting::parseCatalogToml(row(kNewest, ""), kHeadRev);
    TEST_CHECK(entries.size() == 1);
    const auto& e = entries.front();
    TEST_CHECK(e.updatedAt == unixSeconds(0));
    TEST_CHECK(e.addedAt == unixSeconds(0));
  }

  return 0;
}
