#include "system/freedesktop_key_file.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace {

  std::filesystem::path uniqueTestDir() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("noctalia-freedesktop-key-file-test-" + std::to_string(now));
  }

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      std::println(stderr, "freedesktop_key_file_test: {}", message);
    }
    return condition;
  }

  bool hasDiagnostic(const freedesktop::ParseResult& result, std::string_view text) {
    for (const auto& diagnostic : result.diagnostics) {
      if (diagnostic.message.contains(text)) {
        return true;
      }
    }
    return false;
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

  bool ok = true;
  const auto parsed = freedesktop::parseKeyFile(path);
  ok = expect(parsed.has_value(), "valid key file did not parse") && ok;
  if (parsed.has_value()) {
    ok = expect(
             parsed->file.value("Icon Theme", "Directories") == "scalable, 48x48",
             "CRLF assignment retained a stray carriage return"
         )
        && ok;
    ok = expect(
             parsed->file.value("Icon Theme", "Inherits") == "hicolor=legacy",
             "value containing an equals sign was not preserved"
         )
        && ok;
    ok = expect(
             parsed->file.value("48x48", "Label") == "value=with=equals",
             "assignment was not split on the first equals sign"
         )
        && ok;
    ok = expect(parsed->file.value("scalable", "Type") == "Threshold", "duplicate key was not last-wins") && ok;
    ok = expect(parsed->diagnostics.size() == 4, "unexpected diagnostic count") && ok;
    ok = expect(hasDiagnostic(*parsed, "outside a section"), "orphan assignment was not diagnosed") && ok;
    ok = expect(hasDiagnostic(*parsed, "duplicate section"), "duplicate section was not diagnosed") && ok;
    ok = expect(hasDiagnostic(*parsed, "duplicate key"), "duplicate key was not diagnosed") && ok;
    ok = expect(hasDiagnostic(*parsed, "malformed assignment"), "malformed assignment was not diagnosed") && ok;
  }

  const auto invalidUtf8Path = dir / "invalid-utf8.theme";
  {
    std::ofstream file(invalidUtf8Path, std::ios::binary);
    file << "[Icon Theme]\nName=\xFF\n";
  }
  const auto invalidUtf8 = freedesktop::parseKeyFile(invalidUtf8Path);
  ok = expect(!invalidUtf8.has_value(), "invalid UTF-8 was accepted") && ok;
  ok = expect(
           !invalidUtf8.has_value() && invalidUtf8.error().reason == freedesktop::ParseFailure::INVALID_ENCODING,
           "invalid UTF-8 did not return the encoding failure"
       )
      && ok;
  const auto missing = freedesktop::parseKeyFile(dir / "missing.theme");
  ok = expect(!missing.has_value(), "unreadable key file did not fail") && ok;
  ok = expect(
           !missing.has_value() && missing.error().reason == freedesktop::ParseFailure::NOT_FOUND,
           "missing key file did not return the not-found failure"
       )
      && ok;

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return ok ? 0 : 1;
}
