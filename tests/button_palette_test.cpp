#include "render/core/color.h"
#include "ui/controls/button.h"
#include "ui/palette.h"

#include <cmath>
#include <print>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "button_palette_test: {}", message);
      return false;
    }
    return true;
  }

  bool near(float actual, float expected) { return std::abs(actual - expected) < 0.001F; }

} // namespace

int main() {
  bool ok = true;
  const Button::ButtonPalette ghost = Button::defaultPalette(ButtonVariant::Ghost);

  // A role with alpha recolors the normal label; the disabled label keeps the
  // role and multiplies the alpha by the disabled factor.
  {
    const ColorSpec primary = colorSpecFromRole(ColorRole::Primary, 0.2F);
    const Button::ButtonPalette tinted = Button::withLabelColor(ghost, primary);
    ok = expect(tinted.normal.label == primary, "normal label takes the custom color") && ok;
    ok = expect(tinted.disabled.label.role == ColorRole::Primary, "disabled label keeps the custom role") && ok;
    ok = expect(near(tinted.disabled.label.alpha, 0.2F * 0.55F), "disabled alpha multiplies, never replaces") && ok;
    ok = expect(
             tinted.normal.bg == ghost.normal.bg && tinted.normal.border == ghost.normal.border,
             "normal background and border are untouched"
         )
        && ok;
    ok = expect(tinted.hover == ghost.hover, "hover keeps the variant's colors") && ok;
    ok = expect(tinted.pressed == ghost.pressed, "pressed keeps the variant's colors") && ok;
    ok = expect(tinted.selected == ghost.selected, "selected keeps the variant's colors") && ok;
    ok = expect(near(tinted.borderWidth, ghost.borderWidth), "border width is untouched") && ok;
  }

  // A fixed hex color has no role; the disabled factor still lands on the spec's alpha.
  {
    Color fixed;
    ok = expect(tryParseHexColor("#ff8800", fixed), "fixture hex parses") && ok;
    const ColorSpec spec = fixedColorSpec(fixed);
    const Button::ButtonPalette tinted = Button::withLabelColor(ghost, spec);
    ok = expect(tinted.normal.label == spec, "normal label takes the fixed color") && ok;
    ok = expect(!tinted.disabled.label.role.has_value(), "disabled label stays fixed") && ok;
    ok = expect(
             near(tinted.disabled.label.fixed.r, fixed.r)
                 && near(tinted.disabled.label.fixed.g, fixed.g)
                 && near(tinted.disabled.label.fixed.b, fixed.b),
             "disabled label keeps the fixed color"
         )
        && ok;
    ok = expect(near(tinted.disabled.label.alpha, 0.55F), "disabled alpha is the factor for an opaque spec") && ok;
  }

  // palette() reports what setCustomPalette applied, and the variant's own
  // default palette restores the base look.
  {
    Button button;
    button.setVariant(ButtonVariant::Ghost);
    button.setCustomPalette(Button::withLabelColor(ghost, colorSpecFromRole(ColorRole::Primary)));
    ok = expect(
             button.palette().normal.label == colorSpecFromRole(ColorRole::Primary),
             "palette() reflects the custom palette"
         )
        && ok;
    button.setCustomPalette(Button::defaultPalette(button.variant()));
    ok = expect(button.palette() == ghost, "the variant's default palette restores the base") && ok;
  }

  return ok ? 0 : 1;
}
