#include "shell/lockscreen/lockscreen_keyboard_layout.h"
#include "tests/test_check.h"

#include <xkbcommon/xkbcommon-keysyms.h>

using namespace lockscreen_keyboard;

namespace {

  float rowUnits(std::span<const KeyCap> row, std::size_t index) {
    float units = rowInset(index) * 2.0F;
    for (const KeyCap& key : row) {
      units += key.width;
    }
    return units;
  }

  const KeyCap* findAction(Layer layer, KeyAction action) {
    for (const auto& row : rowsFor(layer)) {
      for (const KeyCap& key : row) {
        if (key.action == action) {
          return &key;
        }
      }
    }
    return nullptr;
  }

} // namespace

int main() {
  // Every layer shares one geometry so the UI can relabel buttons in place.
  constexpr Layer kLayers[] = {Layer::Lower, Layer::Upper, Layer::Symbols, Layer::SymbolsAlt};
  for (const Layer layer : kLayers) {
    const auto& rows = rowsFor(layer);
    const auto& base = rowsFor(Layer::Lower);
    for (std::size_t r = 0; r < kRowCount; ++r) {
      TEST_CHECK(rows[r].size() == base[r].size());
      TEST_NEAR(rowUnits(rows[r], r), 10.0F, 1e-4);
      for (std::size_t i = 0; i < rows[r].size(); ++i) {
        TEST_CHECK(rows[r][i].action == base[r][i].action);
        TEST_NEAR(rows[r][i].width, base[r][i].width, 1e-6);
        if (rows[r][i].action == KeyAction::Character) {
          TEST_CHECK(outputFor(rows[r][i]).has_value());
        }
      }
    }
  }

  // Characters report the keysym and codepoint a hardware key would.
  const auto q = outputFor(rowsFor(Layer::Lower)[0][0]);
  TEST_CHECK(q.has_value() && q->sym == XKB_KEY_q && q->utf32 == 'q');
  const auto upperQ = outputFor(rowsFor(Layer::Upper)[0][0]);
  TEST_CHECK(upperQ.has_value() && upperQ->sym == XKB_KEY_Q && upperQ->utf32 == 'Q');
  const auto one = outputFor(rowsFor(Layer::Symbols)[0][0]);
  TEST_CHECK(one.has_value() && one->sym == XKB_KEY_1 && one->utf32 == '1');
  const auto euro = outputFor(KeyCap{.action = KeyAction::Character, .text = "€"});
  TEST_CHECK(euro.has_value() && euro->utf32 == 0x20ACU && euro->sym == XKB_KEY_EuroSign);
  TEST_CHECK(!outputFor(KeyCap{.action = KeyAction::Character, .text = ""}).has_value());
  TEST_CHECK(!outputFor(KeyCap{.action = KeyAction::Character, .text = "\xE2\x82"}).has_value());

  const auto backspace = outputFor(*findAction(Layer::Lower, KeyAction::Backspace));
  TEST_CHECK(backspace.has_value() && backspace->sym == XKB_KEY_BackSpace && backspace->utf32 == 0x08U);
  const auto enter = outputFor(*findAction(Layer::Lower, KeyAction::Enter));
  TEST_CHECK(enter.has_value() && enter->sym == XKB_KEY_Return && enter->utf32 == 0x0DU);
  const auto space = outputFor(*findAction(Layer::Lower, KeyAction::Space));
  TEST_CHECK(space.has_value() && space->sym == XKB_KEY_space && space->utf32 == ' ');
  TEST_CHECK(!outputFor(*findAction(Layer::Lower, KeyAction::Shift)).has_value());
  TEST_CHECK(!outputFor(*findAction(Layer::Lower, KeyAction::LayerToggle)).has_value());
  TEST_CHECK(!outputFor(*findAction(Layer::Lower, KeyAction::Hide)).has_value());

  // Shift is one-shot on letters and pages between the two symbol layers.
  TEST_CHECK(nextLayer(Layer::Lower, KeyAction::Shift) == Layer::Upper);
  TEST_CHECK(nextLayer(Layer::Upper, KeyAction::Shift) == Layer::Lower);
  TEST_CHECK(nextLayer(Layer::Upper, KeyAction::Character) == Layer::Lower);
  TEST_CHECK(nextLayer(Layer::Lower, KeyAction::Character) == Layer::Lower);
  TEST_CHECK(nextLayer(Layer::Symbols, KeyAction::Character) == Layer::Symbols);
  TEST_CHECK(nextLayer(Layer::Symbols, KeyAction::Shift) == Layer::SymbolsAlt);
  TEST_CHECK(nextLayer(Layer::SymbolsAlt, KeyAction::Shift) == Layer::Symbols);
  TEST_CHECK(nextLayer(Layer::Lower, KeyAction::LayerToggle) == Layer::Symbols);
  TEST_CHECK(nextLayer(Layer::Upper, KeyAction::LayerToggle) == Layer::Symbols);
  TEST_CHECK(nextLayer(Layer::SymbolsAlt, KeyAction::LayerToggle) == Layer::Lower);
  TEST_CHECK(nextLayer(Layer::Upper, KeyAction::Backspace) == Layer::Upper);

  // Every special key shows either a label or a glyph.
  constexpr KeyAction kSpecials[] = {KeyAction::Shift, KeyAction::Backspace,   KeyAction::Enter,
                                     KeyAction::Space, KeyAction::LayerToggle, KeyAction::Hide};
  for (const Layer layer : kLayers) {
    for (const KeyAction action : kSpecials) {
      TEST_CHECK(!specialLabel(action, layer).empty() || !specialGlyph(action, layer).empty());
    }
  }

  return 0;
}
