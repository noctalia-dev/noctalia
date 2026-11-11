#include "tests/test_check.h"
#include "theme/kde_color_scheme.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

  // Each check reads its results out inside a block whose TempDir is removed on
  // exit from the block, so a failed check does not leave the directory behind.

  void checkMergePreservesUnrelatedSettings() {
    bool wroteGlobals = false;
    bool wroteScheme = false;
    bool setPermissions = false;
    bool merged = false;
    std::string existingKey;
    std::string colorScheme;
    std::string buttonForeground;
    std::string buttonBackground;
    std::string mixedCase;
    std::string headerForeground;
    std::string globalsContent;
    bool preservedPermissions = false;
    {
      const TempDir root = makeTempDir();
      TEST_CHECK(!root.path.empty());

      const auto scheme = root.path / "noctalia.colors";
      const auto globals = root.path / "kdeglobals";
      wroteGlobals = writeFile(
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
      );
      wroteScheme = writeFile(
          scheme,
          "[General]\n"
          "ColorScheme=noctalia\n"
          "Name=noctalia\n"
          "\n"
          "[Colors:Button]\n"
          "ForegroundNormal=9,8,7\n"
          "BackgroundNormal=4,5,6\n"
          "\n"
          "[Colors:Header][Inactive]\n"
          "ForegroundNormal=6,5,4\n"
      );

      std::error_code permissionsError;
      std::filesystem::permissions(
          globals, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
          std::filesystem::perm_options::replace, permissionsError
      );
      setPermissions = !permissionsError;

      std::string error;
      merged = noctalia::theme::mergeKdeColorScheme(scheme, globals, &error);
      existingKey = keyValue(globals, "General", "ExistingKey");
      colorScheme = keyValue(globals, "General", "ColorScheme");
      buttonForeground = keyValue(globals, "Colors:Button", "ForegroundNormal");
      buttonBackground = keyValue(globals, "Colors:Button", "BackgroundNormal");
      mixedCase = keyValue(globals, "Unrelated", "MixedCase");
      headerForeground = keyValue(globals, "Colors:Header][Inactive", "ForegroundNormal");
      globalsContent = readFile(globals);

      const auto permissions = std::filesystem::status(globals, permissionsError).permissions();
      const auto accessPermissions = permissions
          & (std::filesystem::perms::owner_all
             | std::filesystem::perms::group_all
             | std::filesystem::perms::others_all);
      preservedPermissions = !permissionsError
          && accessPermissions == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }

    TEST_CHECK(wroteGlobals);
    TEST_CHECK(wroteScheme);
    TEST_CHECK(setPermissions);
    TEST_CHECK(merged);
    TEST_CHECK(existingKey == "keep");
    TEST_CHECK(colorScheme == "noctalia");
    TEST_CHECK(buttonForeground == "9,8,7");
    TEST_CHECK(buttonBackground == "4,5,6");
    TEST_CHECK(mixedCase == "Yes");
    // Preserves KDE compound group names.
    TEST_CHECK(headerForeground == "6,5,4");
    TEST_CHECK(globalsContent.contains("# keep this comment"));
    TEST_CHECK(preservedPermissions);
  }

  void checkInvalidSchemeDoesNotOverwriteGlobals() {
    const std::string original = "[General]\nExistingKey=keep\n";
    bool wroteScheme = false;
    bool wroteGlobals = false;
    bool merged = false;
    std::string error;
    std::string globalsContent;
    {
      const TempDir root = makeTempDir();
      TEST_CHECK(!root.path.empty());

      const auto scheme = root.path / "broken.colors";
      const auto globals = root.path / "kdeglobals";
      wroteScheme = writeFile(scheme, "not a key file\n");
      wroteGlobals = writeFile(globals, original);
      merged = noctalia::theme::mergeKdeColorScheme(scheme, globals, &error);
      globalsContent = readFile(globals);
    }

    TEST_CHECK(wroteScheme);
    TEST_CHECK(wroteGlobals);
    TEST_CHECK(!merged);
    TEST_CHECK(!error.empty());
    // Does not overwrite kdeglobals after a parse failure.
    TEST_CHECK(globalsContent == original);
  }

  void checkMissingSchemeFails() {
    bool merged = false;
    std::string error;
    bool globalsExists = false;
    {
      const TempDir root = makeTempDir();
      TEST_CHECK(!root.path.empty());

      merged = noctalia::theme::mergeKdeColorScheme(root.path / "missing.colors", root.path / "kdeglobals", &error);
      globalsExists = std::filesystem::exists(root.path / "kdeglobals");
    }

    TEST_CHECK(!merged);
    TEST_CHECK(!error.empty());
    TEST_CHECK(!globalsExists);
  }

  void checkEmptySchemeFails() {
    bool wroteScheme = false;
    bool merged = false;
    std::string error;
    bool globalsExists = false;
    {
      const TempDir root = makeTempDir();
      TEST_CHECK(!root.path.empty());

      const auto scheme = root.path / "empty.colors";
      const auto globals = root.path / "kdeglobals";
      wroteScheme = writeFile(scheme, "# no color groups\n");
      merged = noctalia::theme::mergeKdeColorScheme(scheme, globals, &error);
      globalsExists = std::filesystem::exists(globals);
    }

    TEST_CHECK(wroteScheme);
    TEST_CHECK(!merged);
    TEST_CHECK(!error.empty());
    TEST_CHECK(!globalsExists);
  }

} // namespace

int main() {
  checkMergePreservesUnrelatedSettings();
  checkInvalidSchemeDoesNotOverwriteGlobals();
  checkMissingSchemeFails();
  checkEmptySchemeFails();
  return 0;
}
