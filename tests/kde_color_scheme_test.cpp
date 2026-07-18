#include "theme/kde_color_scheme.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

  struct TempDir {
    std::filesystem::path path;

    ~TempDir() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  };

  bool fail(std::string_view message) {
    std::println(stderr, "kde_color_scheme_test: FAIL: {}", message);
    return false;
  }

  bool expect(bool condition, std::string_view message) {
    if (!condition) {
      return fail(message);
    }
    return true;
  }

  TempDir makeTempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-kde-colors-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* result = ::mkdtemp(writable.data());
    return TempDir{.path = result != nullptr ? std::filesystem::path(result) : std::filesystem::path{}};
  }

  bool writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path);
    output << content;
    return output.good();
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  std::string keyValue(const std::filesystem::path& path, const char* group, const char* key) {
    std::ifstream input(path);
    std::string currentGroup;
    std::string line;
    while (std::getline(input, line)) {
      if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
        currentGroup = line.substr(1, line.size() - 2);
        continue;
      }
      const std::size_t equals = line.find('=');
      if (currentGroup == group && equals != std::string::npos && line.substr(0, equals) == key) {
        return line.substr(equals + 1);
      }
    }
    return {};
  }

  bool checkMergePreservesUnrelatedSettings() {
    const TempDir root = makeTempDir();
    if (!expect(!root.path.empty(), "creates a temporary directory")) {
      return false;
    }

    const auto scheme = root.path / "noctalia.colors";
    const auto globals = root.path / "kdeglobals";
    bool ok = expect(
        writeFile(
            globals,
            "# keep this comment\n"
            "[General]\n"
            "ExistingKey=keep\n"
            "ColorScheme=Old\n"
            "\n"
            "[Colors:Button]\n"
            "ForegroundNormal=1,2,3\n"
            "\n"
            "[Unrelated]\n"
            "MixedCase=Yes\n"
        ),
        "writes the existing kdeglobals fixture"
    );
    ok = expect(
             writeFile(
                 scheme,
                 "[General]\n"
                 "ColorScheme=Noctalia\n"
                 "Name=noctalia\n"
                 "\n"
                 "[Colors:Button]\n"
                 "ForegroundNormal=9,8,7\n"
                 "BackgroundNormal=4,5,6\n"
                 "\n"
                 "[Colors:Header][Inactive]\n"
                 "ForegroundNormal=6,5,4\n"
             ),
             "writes the color-scheme fixture"
         )
        && ok;

    std::error_code permissionsError;
    std::filesystem::permissions(
        globals, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, permissionsError
    );
    ok = expect(!permissionsError, "sets restrictive kdeglobals permissions") && ok;

    std::string error;
    const bool merged = noctalia::theme::mergeKdeColorScheme(scheme, globals, &error);
    ok = expect(merged, error) && ok;
    ok = expect(keyValue(globals, "General", "ExistingKey") == "keep", "preserves an unrelated key") && ok;
    ok = expect(keyValue(globals, "General", "ColorScheme") == "Noctalia", "replaces the active scheme") && ok;
    ok = expect(keyValue(globals, "Colors:Button", "ForegroundNormal") == "9,8,7", "replaces scheme values") && ok;
    ok = expect(keyValue(globals, "Colors:Button", "BackgroundNormal") == "4,5,6", "adds scheme values") && ok;
    ok =
        expect(keyValue(globals, "Unrelated", "MixedCase") == "Yes", "preserves key casing and unrelated groups") && ok;
    ok = expect(
             keyValue(globals, "Colors:Header][Inactive", "ForegroundNormal") == "6,5,4",
             "preserves KDE compound group names"
         )
        && ok;
    ok = expect(readFile(globals).contains("# keep this comment"), "preserves comments") && ok;

    const auto permissions = std::filesystem::status(globals, permissionsError).permissions();
    const auto accessPermissions = permissions
        & (std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all);
    ok = expect(
             !permissionsError
                 && accessPermissions == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
             "preserves file permissions"
         )
        && ok;
    return ok;
  }

  bool checkInvalidSchemeDoesNotOverwriteGlobals() {
    const TempDir root = makeTempDir();
    if (!expect(!root.path.empty(), "creates a temporary directory for invalid input")) {
      return false;
    }

    const auto scheme = root.path / "broken.colors";
    const auto globals = root.path / "kdeglobals";
    const std::string original = "[General]\nExistingKey=keep\n";
    bool ok = expect(writeFile(scheme, "not a key file\n"), "writes malformed scheme fixture");
    ok = expect(writeFile(globals, original), "writes protected kdeglobals fixture") && ok;

    std::string error;
    ok = expect(!noctalia::theme::mergeKdeColorScheme(scheme, globals, &error), "rejects a malformed scheme") && ok;
    ok = expect(!error.empty(), "reports the malformed scheme error") && ok;
    ok = expect(readFile(globals) == original, "does not overwrite kdeglobals after a parse failure") && ok;
    return ok;
  }

  bool checkMissingSchemeFails() {
    const TempDir root = makeTempDir();
    if (!expect(!root.path.empty(), "creates a temporary directory for missing input")) {
      return false;
    }

    std::string error;
    const bool merged =
        noctalia::theme::mergeKdeColorScheme(root.path / "missing.colors", root.path / "kdeglobals", &error);
    return expect(!merged, "rejects a missing scheme")
        && expect(!error.empty(), "reports the missing scheme error")
        && expect(!std::filesystem::exists(root.path / "kdeglobals"), "does not create kdeglobals for missing input");
  }

  bool checkEmptySchemeFails() {
    const TempDir root = makeTempDir();
    if (!expect(!root.path.empty(), "creates a temporary directory for empty input")) {
      return false;
    }

    const auto scheme = root.path / "empty.colors";
    const auto globals = root.path / "kdeglobals";
    bool ok = expect(writeFile(scheme, "# no color groups\n"), "writes empty scheme fixture");
    std::string error;
    ok = expect(!noctalia::theme::mergeKdeColorScheme(scheme, globals, &error), "rejects an empty scheme") && ok;
    ok = expect(!error.empty(), "reports the empty scheme error") && ok;
    ok = expect(!std::filesystem::exists(globals), "does not create kdeglobals for empty input") && ok;
    return ok;
  }

} // namespace

int main() {
  bool ok = true;
  ok = checkMergePreservesUnrelatedSettings() && ok;
  ok = checkInvalidSchemeDoesNotOverwriteGlobals() && ok;
  ok = checkMissingSchemeFails() && ok;
  ok = checkEmptySchemeFails() && ok;
  return ok ? 0 : 1;
}
