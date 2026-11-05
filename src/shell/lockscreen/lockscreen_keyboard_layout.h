#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// Pure layout and keymap data for the lockscreen on-screen keyboard. Kept free of scene code so the
// key -> keysym mapping and layer transitions are unit-testable.
namespace lockscreen_keyboard {

  enum class KeyAction : std::uint8_t {
    Character,
    Shift, // Lower <-> Upper; Symbols <-> SymbolsAlt
    Backspace,
    Enter,
    Space,
    LayerToggle, // letters <-> symbols
    Hide,
  };

  enum class Layer : std::uint8_t {
    Lower,
    Upper,
    Symbols,
    SymbolsAlt,
  };

  struct KeyCap {
    KeyAction action = KeyAction::Character;
    // UTF-8 text for Character keys; empty for special keys (their label depends on the layer).
    std::string_view text;
    // Width in key units (1.0 is a regular letter key).
    float width = 1.0F;
  };

  constexpr std::size_t kRowCount = 4;

  // Every layer has the same number of keys per row and the same special keys at the same positions,
  // so the UI can build its buttons once and only relabel them when the layer changes.
  using LayerRows = std::array<std::span<const KeyCap>, kRowCount>;

  [[nodiscard]] const LayerRows& rowsFor(Layer layer) noexcept;

  // Indent, in key units, applied on both sides of a row (row 1 is offset by half a key).
  [[nodiscard]] float rowInset(std::size_t row) noexcept;

  struct KeyOutput {
    std::uint32_t sym = 0;
    std::uint32_t utf32 = 0;
  };

  // The keysym/codepoint pair a hardware keyboard would report for this key, or nullopt for keys
  // that only change the keyboard's own state (Shift, LayerToggle, Hide).
  [[nodiscard]] std::optional<KeyOutput> outputFor(const KeyCap& key) noexcept;

  // The layer to show after `action` is pressed on `current`. Upper is one-shot: typing a character
  // returns to Lower.
  [[nodiscard]] Layer nextLayer(Layer current, KeyAction action) noexcept;

  // Text label for special keys on `layer` (empty when the key shows a glyph instead).
  [[nodiscard]] std::string_view specialLabel(KeyAction action, Layer layer) noexcept;
  // Glyph name for special keys (empty when the key shows text).
  [[nodiscard]] std::string_view specialGlyph(KeyAction action, Layer layer) noexcept;

} // namespace lockscreen_keyboard
