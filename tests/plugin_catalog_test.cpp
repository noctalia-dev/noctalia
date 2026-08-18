#include "scripting/plugin_api.h"
#include "scripting/plugin_catalog.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <print>
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

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "plugin_catalog_test: {}", message);
    }
    return condition;
  }

  bool expectEq(std::string_view actual, std::string_view expected, const char* message) {
    if (actual != expected) {
      std::println(stderr, "plugin_catalog_test: {}\n  actual:   {}\n  expected: {}", message, actual, expected);
      return false;
    }
    return true;
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
  bool ok = true;

  {
    // A supported tip wins outright: no release is consulted and the export targets the
    // revision the catalog itself was read from.
    const auto entries = scripting::parseCatalogToml(row(kNewest, release(kOldest, "1.0.0", kOldestRev)), kHeadRev);
    ok = expect(entries.size() == 1, "a well-formed row should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.compatible, "a tip inside the supported range is compatible") && ok;
      ok = expect(!e.heldBack, "a supported tip is not held back") && ok;
      ok = expectEq(e.resolvedVersion, "2.0.0", "a supported tip resolves to its own version") && ok;
      ok = expectEq(e.resolvedRevision, kHeadRev, "a supported tip resolves to the catalog revision") && ok;
      ok = expect(e.resolvedPluginApiVersion == kNewest, "a supported tip resolves to its own API level") && ok;
    }
  }

  {
    // The whole point: a tip above the range falls back to the newest release below it.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", kNewestRev)), kHeadRev);
    ok = expect(entries.size() == 1, "a row with an unsupported tip should still parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.compatible, "an unsupported tip with a usable release is installable") && ok;
      ok = expect(e.heldBack, "an unsupported tip with a usable release is held back") && ok;
      ok = expectEq(e.resolvedVersion, "1.9.0", "a held-back row resolves to the release version") && ok;
      ok = expectEq(e.resolvedRevision, kNewestRev, "a held-back row resolves to the release revision") && ok;
      ok = expect(e.resolvedPluginApiVersion == kNewest, "a held-back row resolves to the release API level") && ok;
      ok = expectEq(e.version, "2.0.0", "the tip version stays readable for the UI") && ok;
      ok = expect(e.pluginApiVersion == kTooNew, "the tip API level stays readable for the UI") && ok;
    }
  }

  {
    // Highest supported release wins, and declaration order must not decide it.
    const std::string releases = release(kOldest, "1.0.0", kOldestRev) + release(kNewest, "1.9.0", kNewestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, releases), kHeadRev);
    ok = expect(entries.size() == 1, "a row with two releases should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.releases.size() == 2, "both releases should be kept") && ok;
      ok = expectEq(e.resolvedVersion, "1.9.0", "the newest supported release wins regardless of order") && ok;
    }
  }

  {
    // Nothing in range in either direction: the row stays uninstallable rather than
    // silently landing a version the host cannot run.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kTooOld, "0.1.0", kOldestRev)), kHeadRev);
    ok = expect(entries.size() == 1, "a row whose releases are all out of range should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(!e.compatible, "a row with no usable release is incompatible") && ok;
      ok = expect(!e.heldBack, "an incompatible row is not held back") && ok;
      ok = expectEq(e.resolvedVersion, "2.0.0", "an incompatible row still reports the tip version") && ok;
    }
  }

  {
    // A revision is interpolated into a git command line, so anything that is not a full
    // sha is dropped and the next release is used instead.
    const std::string releases = release(kNewest, "1.9.0", "not-a-sha") + release(kOldest, "1.0.0", kOldestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, releases), kHeadRev);
    ok = expect(entries.size() == 1, "a row with a malformed release rev should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.releases.size() == 1, "a release with a malformed rev is dropped") && ok;
      ok = expectEq(e.resolvedVersion, "1.0.0", "resolution skips the release with a malformed rev") && ok;
    }
  }

  {
    // An uppercase sha is not what git prints, so it is malformed too.
    const std::string upper(40, 'A');
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", upper)), kHeadRev);
    ok = expect(entries.size() == 1, "a row with an uppercase release rev should parse") && ok;
    if (entries.size() == 1) {
      ok = expect(entries.front().releases.empty(), "an uppercase release rev is rejected") && ok;
      ok = expect(!entries.front().compatible, "a row whose only release is rejected is incompatible") && ok;
    }
  }

  {
    // A release at or above the tip can never be the answer, so it is a catalog error.
    const auto entries = scripting::parseCatalogToml(row(kNewest, release(kNewest, "1.9.0", kNewestRev)), kHeadRev);
    ok = expect(entries.size() == 1, "a row with a release at the tip level should parse") && ok;
    if (entries.size() == 1) {
      ok = expect(entries.front().releases.empty(), "a release at the tip's API level is dropped") && ok;
    }
  }

  {
    // A release without a version cannot be compared against the exported copy on disk.
    const std::string bad = std::format("[[plugin.release]]\nplugin_api = {}\nrev = \"{}\"\n", kNewest, kNewestRev);
    const auto entries = scripting::parseCatalogToml(row(kTooNew, bad), kHeadRev);
    ok = expect(entries.size() == 1, "a row with a versionless release should parse") && ok;
    if (entries.size() == 1) {
      ok = expect(entries.front().releases.empty(), "a release without a version is dropped") && ok;
    }
  }
  {
    const auto entries = scripting::parseCatalogToml(row(kNewest, "", "1.2.3-beta.1"), kHeadRev);
    ok = expect(entries.empty(), "a catalog row with a non-canonical version should be dropped") && ok;
  }

  {
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "01.9.0", kNewestRev)), kHeadRev);
    ok = expect(entries.size() == 1, "an invalid release version should not drop its catalog row") && ok;
    if (entries.size() == 1) {
      ok = expect(entries.front().releases.empty(), "a release with a non-canonical version is dropped") && ok;
      ok = expect(!entries.front().compatible, "an invalid release cannot satisfy compatibility") && ok;
    }
  }

  {
    // A path source has no revisions to export, so its releases never apply.
    const auto entries = scripting::parseCatalogToml(row(kTooNew, release(kNewest, "1.9.0", kNewestRev)), "");
    ok = expect(entries.size() == 1, "a path-source row should parse") && ok;
    if (entries.size() == 1) {
      ok = expect(!entries.front().compatible, "a path source ignores releases and judges the tip alone") && ok;
      ok = expect(!entries.front().heldBack, "a path source is never held back") && ok;
    }
  }

  {
    // The store sorts on these two, so they must survive the parse as unix seconds.
    const std::string dated = row(kNewest, "") + "updated_at = 1750000000\nadded_at = 1700000000\n";
    const auto entries = scripting::parseCatalogToml(dated, kHeadRev);
    ok = expect(entries.size() == 1, "a row carrying dates should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.updatedAt == unixSeconds(1750000000), "updated_at is read as unix seconds") && ok;
      ok = expect(e.addedAt == unixSeconds(1700000000), "added_at is read as unix seconds") && ok;
    }
  }

  {
    // Sources predating the date fields must still parse; the store falls back to a name
    // tiebreak when every entry lands on the epoch.
    const auto entries = scripting::parseCatalogToml(row(kNewest, ""), kHeadRev);
    ok = expect(entries.size() == 1, "a row without dates should parse") && ok;
    if (entries.size() == 1) {
      const auto& e = entries.front();
      ok = expect(e.updatedAt == unixSeconds(0), "a missing updated_at reads as the epoch") && ok;
      ok = expect(e.addedAt == unixSeconds(0), "a missing added_at reads as the epoch") && ok;
    }
  }

  return ok ? 0 : 1;
}
