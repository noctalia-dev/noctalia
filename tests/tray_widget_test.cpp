#include "shell/bar/widgets/tray_widget.h"
#include "tests/test_check.h"

#include <string_view>

class TrayWidgetTestAccess {
public:
  static std::string_view resolveGlyph(
      std::string_view glyph, std::string_view glyphExpanded, bool drawerMode, std::string_view barPosition,
      bool isVertical, GlyphPosition glyphPosition, bool expanded
  ) {
    return TrayWidget::resolveGlyph(glyph, glyphExpanded, drawerMode, barPosition, isVertical, glyphPosition, expanded);
  }
};

int main() {
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "top", false, GlyphPosition::Before, false) == "chevron-down"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "top", false, GlyphPosition::Before, true) == "chevron-up"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "bottom", false, GlyphPosition::Before, false) == "chevron-up"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "bottom", false, GlyphPosition::Before, true) == "chevron-down"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "left", true, GlyphPosition::Before, false) == "chevron-right"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "left", true, GlyphPosition::Before, true) == "chevron-left"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "right", true, GlyphPosition::Before, false) == "chevron-left"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", true, "right", true, GlyphPosition::Before, true) == "chevron-right"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "top", false, GlyphPosition::Before, false) == "chevron-right"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "top", false, GlyphPosition::Before, true) == "chevron-left"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "top", false, GlyphPosition::After, false) == "chevron-left"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "top", false, GlyphPosition::After, true) == "chevron-right"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "left", true, GlyphPosition::Before, false) == "chevron-down"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "left", true, GlyphPosition::Before, true) == "chevron-up"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "left", true, GlyphPosition::After, false) == "chevron-up"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "", false, "left", true, GlyphPosition::After, true) == "chevron-down"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("custom-tray", "", true, "top", false, GlyphPosition::Before, false)
      == "custom-tray"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("custom-tray", "", true, "top", false, GlyphPosition::Before, true)
      == "custom-tray"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("custom-tray", "", false, "top", false, GlyphPosition::Before, false)
      == "custom-tray"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("custom-tray", "", false, "top", false, GlyphPosition::Before, true)
      == "custom-tray"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("closed-icon", "open-icon", true, "top", false, GlyphPosition::Before, false)
      == "closed-icon"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("closed-icon", "open-icon", true, "top", false, GlyphPosition::Before, true)
      == "open-icon"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("closed-icon", "open-icon", false, "top", false, GlyphPosition::Before, false)
      == "closed-icon"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("closed-icon", "open-icon", false, "top", false, GlyphPosition::Before, true)
      == "open-icon"
  );

  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "open-icon", true, "top", false, GlyphPosition::Before, false)
      == "chevron-down"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "open-icon", true, "top", false, GlyphPosition::Before, true)
      == "open-icon"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "open-icon", false, "top", false, GlyphPosition::Before, false)
      == "chevron-right"
  );
  TEST_CHECK(
      TrayWidgetTestAccess::resolveGlyph("", "open-icon", false, "top", false, GlyphPosition::Before, true)
      == "open-icon"
  );

  return 0;
}
