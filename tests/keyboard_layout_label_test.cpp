#include "shell/keyboard_layout_label.h"
#include "tests/test_check.h"

#include <string>
#include <unordered_map>

int main() {
  const std::unordered_map<std::string, std::string> customLabels{
      {"English (US)", "Work"},
      {"German", ""},
  };

  TEST_CHECK(resolveKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Short, customLabels) == "Work");
  TEST_CHECK(resolveKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Full, customLabels) == "Work");
  TEST_CHECK(resolveKeyboardLayoutLabel("German", KeyboardLayoutDisplayMode::Short, customLabels) == "DE");
  TEST_CHECK(formatKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Full) == "English (US)");
  TEST_CHECK(formatKeyboardLayoutLabel("English (UK, Macintosh)", KeyboardLayoutDisplayMode::Short) == "GB");
  TEST_CHECK(formatKeyboardLayoutLabel("English (UK)", KeyboardLayoutDisplayMode::Short) == "GB");
  TEST_CHECK(
      formatKeyboardLayoutLabel("English (UK, Macintosh)", KeyboardLayoutDisplayMode::Full) == "English (UK, Macintosh)"
  );
  TEST_CHECK(formatKeyboardLayoutLabel("English (Macintosh, ABC, ANSI)", KeyboardLayoutDisplayMode::Short) == "US");
  TEST_CHECK(formatKeyboardLayoutLabel("unknown layout", KeyboardLayoutDisplayMode::Short) == "--");

  return 0;
}
