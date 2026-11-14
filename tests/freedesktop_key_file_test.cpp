#include "system/freedesktop_key_file.h"
#include "tests/test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

  std::filesystem::path uniqueTestDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("noctalia-freedesktop-key-file-test-" + std::to_string(now));
  }

  bool hasDiagnostic(const freedesktop::ParseResult& result, std::string_view text) {
    return std::ranges::any_of(result.diagnostics, [text](const auto& diagnostic) {
      return diagnostic.message.contains(text);
    });
  }

} // namespace

int main() {
  const auto dir = uniqueTestDir();
  std::filesystem::create_directories(dir);
  const auto path = dir / "index.theme";
  {
    std::ofstream file(path, std::ios::binary);
    file << "\xEF\xBB\xBF# comment\r\n"
            "outside=ignored\n"
            "[Icon Theme]\n"
            "Directories = scalable, 48x48\r\n"
            "Inherits = hicolor=legacy\n"
            "[scalable]\n"
            "Type = Scalable\n"
            "[scalable]\n"
            "Type = Threshold\n"
            "bad line\n"
            "[48x48]\n"
            "Size = 48\n"
            "Label = value=with=equals\n";
  }
  const auto invalidUtf8Path = dir / "invalid-utf8.theme";
  {
    std::ofstream file(invalidUtf8Path, std::ios::binary);
    file << "[Icon Theme]\nName=\xFF\n";
  }

  const auto parsed = freedesktop::parseKeyFile(path);
  const auto invalidUtf8 = freedesktop::parseKeyFile(invalidUtf8Path);
  const auto missing = freedesktop::parseKeyFile(dir / "missing.theme");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  TEST_CHECK(parsed.has_value());
  // CRLF assignment must not retain a stray carriage return
  TEST_CHECK(parsed->file.value("Icon Theme", "Directories") == "scalable, 48x48");
  TEST_CHECK(parsed->file.value("Icon Theme", "Inherits") == "hicolor=legacy");
  // Assignment is split on the first equals sign
  TEST_CHECK(parsed->file.value("48x48", "Label") == "value=with=equals");
  // Duplicate key is last-wins
  TEST_CHECK(parsed->file.value("scalable", "Type") == "Threshold");
  TEST_CHECK(parsed->diagnostics.size() == 4);
  TEST_CHECK(hasDiagnostic(*parsed, "outside a section"));
  TEST_CHECK(hasDiagnostic(*parsed, "duplicate section"));
  TEST_CHECK(hasDiagnostic(*parsed, "duplicate key"));
  TEST_CHECK(hasDiagnostic(*parsed, "malformed assignment"));
  TEST_CHECK(parsed->file.sectionNames() == std::vector<std::string>{"Icon Theme", "scalable", "48x48"});

  const auto inMemory = freedesktop::parseKeyFileContents("[b]\nk=v\n[a]\nk=w");
  TEST_CHECK(inMemory.has_value());
  // Final line without a newline must not be dropped
  TEST_CHECK(inMemory->file.value("a", "k") == "w");
  TEST_CHECK(inMemory->file.sectionNames() == std::vector<std::string>{"b", "a"});
  TEST_CHECK(!freedesktop::parseKeyFileContents("[a]\nk=\xFF").has_value());

  TEST_CHECK(freedesktop::unescapeValue(R"(a\sb\nc\td\re\\f\qg\)") == "a b\nc\td\re\\f\\qg\\");

  TEST_CHECK(!invalidUtf8.has_value());
  TEST_CHECK(invalidUtf8.error().reason == freedesktop::ParseFailure::InvalidEncoding);
  TEST_CHECK(!missing.has_value());
  TEST_CHECK(missing.error().reason == freedesktop::ParseFailure::NotFound);

  return 0;
}
