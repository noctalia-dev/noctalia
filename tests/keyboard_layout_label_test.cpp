#include "shell/keyboard_layout_label.h"

#include <print>
#include <string>
#include <unordered_map>

namespace {

  bool g_ok = true;

  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "keyboard_layout_label_test: FAIL: {}", message);
      g_ok = false;
    }
  }

} // namespace

int main() {
  const std::unordered_map<std::string, std::string> customLabels{
      {"English (US)", "Work"},
      {"German", ""},
  };

  expect(
      resolveKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Short, customLabels) == "Work",
      "custom label did not override short formatting"
  );
  expect(
      resolveKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Full, customLabels) == "Work",
      "custom label did not override full formatting"
  );
  expect(
      resolveKeyboardLayoutLabel("German", KeyboardLayoutDisplayMode::Short, customLabels) == "DE",
      "empty custom label did not fall back to short formatting"
  );
  expect(
      formatKeyboardLayoutLabel("English (US)", KeyboardLayoutDisplayMode::Full) == "English (US)",
      "full formatting changed the layout name"
  );
  expect(
      formatKeyboardLayoutLabel("unknown layout", KeyboardLayoutDisplayMode::Short) == "--",
      "unknown short layout did not use the unknown marker"
  );

  return g_ok ? 0 : 1;
}
