#include "launcher/math_provider.h"
#include "tests/test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  namespace fs = std::filesystem;

  std::string firstTitle(const MathProvider& provider, std::string_view text) {
    const auto results = provider.queryPrefixed(text);
    return results.empty() ? std::string() : results.front().title;
  }

  void writeVariable(const fs::path& definitions, std::string_view name, std::string_view value) {
    std::ofstream file(definitions / "variables.xml");
    file
        << "<?xml version=\"1.0\"?>\n<QALCULATE version=\"5.13.1\">\n  <variable>\n    <names>r:"
        << name
        << "</names>\n    <value>"
        << value
        << "</value>\n  </variable>\n</QALCULATE>\n";
  }

  // command() runs a program; it must stay disabled even after user definitions load.
  bool commandRuns(const MathProvider& provider, const fs::path& marker) {
    (void)provider.queryPrefixed(R"(command("touch", ")" + marker.string() + R"("))");
    return fs::exists(marker);
  }

} // namespace

int main() {
  // Keep libqalculate away from the real definitions and exchange rates.
  const fs::path root = fs::temp_directory_path() / ("noctalia-math-provider-" + std::to_string(getpid()));
  const fs::path definitions = root / "definitions";
  const fs::path marker = root / "command-ran";
  fs::create_directories(root);
  setenv("QALCULATE_USER_DIR", root.c_str(), 1);

  // Starts without a definitions directory, so no watch is set up.
  MathProvider provider(nullptr, nullptr, nullptr);
  provider.initialize();

  TEST_CHECK(provider.query("EUR").empty());
  TEST_CHECK(!provider.queryPrefixed("EUR").empty());
  TEST_CHECK(!provider.query("2 + 2").empty());
  TEST_CHECK(!commandRuns(provider, marker));

  // A definitions directory created after startup is picked up on the next open.
  TEST_CHECK(firstTitle(provider, "noctaliaprobe*2") != "= 480");
  fs::create_directories(definitions);
  writeVariable(definitions, "noctaliaprobe", "240");
  provider.onLauncherOpened();
  TEST_CHECK(firstTitle(provider, "noctaliaprobe*2") == "= 480");
  TEST_CHECK(!commandRuns(provider, marker));

  // Edits are picked up through the watch.
  writeVariable(definitions, "noctaliaprobe", "100");
  provider.onLauncherOpened();
  TEST_CHECK(firstTitle(provider, "noctaliaprobe*2") == "= 200");

  // Removed definitions disappear rather than lingering from the previous load.
  fs::remove(definitions / "variables.xml");
  provider.onLauncherOpened();
  TEST_CHECK(firstTitle(provider, "noctaliaprobe*2") != "= 200");
  TEST_CHECK(!commandRuns(provider, marker));

  std::error_code ec;
  fs::remove_all(root, ec);
  return 0;
}
