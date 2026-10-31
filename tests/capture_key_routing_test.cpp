// While a region capture is active, every key press is answered by the capture itself: the
// confirm and cancel keys act, and any other press is swallowed rather than passed on. On niri
// the launcher keeps the compositor's keyboard focus under the overlay, so a key that was not
// consumed used to land in the launcher's search field or launch its selected app (#4641).

#include "capture/capture_key_routing.h"
#include "core/input/key_modifiers.h"
#include "core/input/keybind_matcher.h"
#include "tests/test_check.h"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  using capture::CaptureKeyAction;
  using capture::regionCaptureKeyAction;

  constexpr bool kPressed = true;
  constexpr bool kReleased = false;
  constexpr bool kConfirming = true;
  constexpr bool kSelecting = false;

  // The default chords: Escape cancels, Ctrl+C copies, Ctrl+S saves.
  void installDefaultMatchers() {
    KeybindMatcher::setMatcher(KeybindAction::Cancel, [](std::uint32_t sym, std::uint32_t) {
      return sym == XKB_KEY_Escape;
    });
    KeybindMatcher::setMatcher(KeybindAction::Copy, [](std::uint32_t sym, std::uint32_t modifiers) {
      return (sym == XKB_KEY_c || sym == XKB_KEY_C) && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::Save, [](std::uint32_t sym, std::uint32_t modifiers) {
      return (sym == XKB_KEY_s || sym == XKB_KEY_S) && modifiers == KeyMod::Ctrl;
    });
  }

} // namespace

int main() {
  installDefaultMatchers();

  // A release is not the capture's to answer.
  TEST_CHECK(regionCaptureKeyAction(kReleased, XKB_KEY_Return, 0, kConfirming) == CaptureKeyAction::Pass);
  TEST_CHECK(regionCaptureKeyAction(kReleased, XKB_KEY_a, 0, kSelecting) == CaptureKeyAction::Pass);

  // Cancel works at every stage.
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_Escape, 0, kSelecting) == CaptureKeyAction::Cancel);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_Escape, 0, kConfirming) == CaptureKeyAction::Cancel);

  // On a pending selection, Enter and Space confirm and the copy and save chords force an output.
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_Return, 0, kConfirming) == CaptureKeyAction::Confirm);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_KP_Enter, 0, kConfirming) == CaptureKeyAction::Confirm);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_space, 0, kConfirming) == CaptureKeyAction::Confirm);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_c, KeyMod::Ctrl, kConfirming) == CaptureKeyAction::Copy);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_s, KeyMod::Ctrl, kConfirming) == CaptureKeyAction::Save);

  // Before a selection is pending, the same keys do nothing, and still never fall through.
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_Return, 0, kSelecting) == CaptureKeyAction::Swallow);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_space, 0, kSelecting) == CaptureKeyAction::Swallow);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_c, KeyMod::Ctrl, kSelecting) == CaptureKeyAction::Swallow);

  // Typing is swallowed at every stage: it must not reach a panel's search field.
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_a, 0, kSelecting) == CaptureKeyAction::Swallow);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_a, 0, kConfirming) == CaptureKeyAction::Swallow);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_a, KeyMod::Shift, kConfirming) == CaptureKeyAction::Swallow);
  TEST_CHECK(regionCaptureKeyAction(kPressed, XKB_KEY_Tab, 0, kConfirming) == CaptureKeyAction::Swallow);

  return 0;
}
