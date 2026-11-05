#include "core/input/key_modifiers.h"
#include "shell/switcher/window_switcher_shortcut_state.h"
#include "tests/test_check.h"

namespace {

  void testRealReleaseConfirmsAfterModifiersClear() {
    WindowSwitcherShortcutState state;
    state.capture(KeyMod::Alt);

    TEST_CHECK(state.noteRelease(KeyMod::Alt, KeyMod::Alt));
    TEST_CHECK(state.beginReleaseCheck());
    TEST_CHECK(!state.beginReleaseCheck());
    TEST_CHECK(state.completeReleaseCheck(0));
  }

  void testInputMethodReleaseDoesNotConfirmRestoredModifier() {
    WindowSwitcherShortcutState state;
    state.capture(KeyMod::Super);

    TEST_CHECK(state.noteRelease(KeyMod::Super, KeyMod::Super));
    TEST_CHECK(state.beginReleaseCheck());
    state.updateModifiers(0);
    state.updateModifiers(KeyMod::Super);
    TEST_CHECK(!state.beginReleaseCheck());
    TEST_CHECK(!state.completeReleaseCheck(KeyMod::Super));

    state.updateModifiers(0);
    TEST_CHECK(state.beginReleaseCheck());
    TEST_CHECK(state.noteRelease(KeyMod::Super, 0));
    TEST_CHECK(!state.beginReleaseCheck());
    TEST_CHECK(state.completeReleaseCheck(0));
  }

  void testPhysicalReleaseDuringPendingCheckConfirms() {
    WindowSwitcherShortcutState state;
    state.capture(KeyMod::Super);

    TEST_CHECK(state.noteRelease(KeyMod::Super, KeyMod::Super));
    TEST_CHECK(state.beginReleaseCheck());

    state.updateModifiers(0);
    TEST_CHECK(state.noteRelease(KeyMod::Super, 0));
    TEST_CHECK(!state.beginReleaseCheck());
    TEST_CHECK(state.completeReleaseCheck(0));
  }

  void testReleaseCapturesModifierFromReleasedKey() {
    WindowSwitcherShortcutState state;

    TEST_CHECK(state.noteRelease(KeyMod::Alt, 0));
    TEST_CHECK(state.modifiers() == KeyMod::Alt);

    TEST_CHECK(state.beginReleaseCheck());
    state.updateModifiers(KeyMod::Alt);
    TEST_CHECK(!state.completeReleaseCheck(KeyMod::Alt));
  }

  void testUnrelatedReleaseIsIgnored() {
    WindowSwitcherShortcutState state;
    state.capture(KeyMod::Super);

    TEST_CHECK(!state.noteRelease(KeyMod::Alt, KeyMod::Super));
    TEST_CHECK(!state.hasPendingRelease());
    TEST_CHECK(!state.beginReleaseCheck());
  }

  void testResetInvalidatesPendingRelease() {
    WindowSwitcherShortcutState state;
    state.capture(KeyMod::Ctrl | KeyMod::Alt);
    TEST_CHECK(state.noteRelease(KeyMod::Alt, KeyMod::Ctrl | KeyMod::Alt));
    TEST_CHECK(state.beginReleaseCheck());

    state.reset();
    TEST_CHECK(!state.hasPendingRelease());
    TEST_CHECK(!state.completeReleaseCheck(0));
    TEST_CHECK(!state.beginReleaseCheck());
  }

  void testFocusCheckWithoutHeldModifierConfirms() {
    WindowSwitcherShortcutState state;
    TEST_CHECK(!state.beginReleaseCheck());

    state.expectHeldModifier();
    TEST_CHECK(state.beginReleaseCheck());
    TEST_CHECK(state.completeReleaseCheck(KeyMod::Shift));
    TEST_CHECK(!state.hasPendingFocusCheck());
  }

  void testFocusCheckWithHeldModifierWaitsForRelease() {
    WindowSwitcherShortcutState state;
    state.expectHeldModifier();

    TEST_CHECK(state.beginReleaseCheck());
    TEST_CHECK(!state.completeReleaseCheck(KeyMod::Super));
    TEST_CHECK(state.modifiers() == KeyMod::Super);
    TEST_CHECK(!state.beginReleaseCheck());

    TEST_CHECK(state.noteRelease(KeyMod::Super, 0));
    TEST_CHECK(state.beginReleaseCheck());
    TEST_CHECK(state.completeReleaseCheck(0));
  }

  void testResetClearsFocusCheck() {
    WindowSwitcherShortcutState state;
    state.expectHeldModifier();
    state.reset();
    TEST_CHECK(!state.hasPendingFocusCheck());
    TEST_CHECK(!state.beginReleaseCheck());
  }

} // namespace

int main() {
  testRealReleaseConfirmsAfterModifiersClear();
  testInputMethodReleaseDoesNotConfirmRestoredModifier();
  testPhysicalReleaseDuringPendingCheckConfirms();
  testReleaseCapturesModifierFromReleasedKey();
  testUnrelatedReleaseIsIgnored();
  testResetInvalidatesPendingRelease();
  testFocusCheckWithoutHeldModifierConfirms();
  testFocusCheckWithHeldModifierWaitsForRelease();
  testResetClearsFocusCheck();
  return 0;
}
