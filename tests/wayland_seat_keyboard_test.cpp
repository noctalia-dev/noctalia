#include "test_isolation.h"
#include "wayland/wayland_seat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <linux/input-event-codes.h>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace {

  void loadKeymap(WaylandSeat& seat, const char* variant) {
    const std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context(
        xkb_context_new(XKB_CONTEXT_NO_FLAGS), xkb_context_unref
    );
    TEST_CHECK(context != nullptr);
    const xkb_rule_names rules{.rules = "evdev", .model = "pc105", .layout = "us", .variant = variant, .options = ""};
    const std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> keymap(
        xkb_keymap_new_from_names(context.get(), &rules, XKB_KEYMAP_COMPILE_NO_FLAGS), xkb_keymap_unref
    );
    TEST_CHECK(keymap != nullptr);
    const std::unique_ptr<char, decltype(&std::free)> keymapText(
        xkb_keymap_get_as_string(keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1), std::free
    );
    TEST_CHECK(keymapText != nullptr);
    const auto size = std::strlen(keymapText.get()) + 1;
    const auto closeFile = [](FILE* file) { std::fclose(file); };
    const std::unique_ptr<FILE, decltype(closeFile)> file(std::tmpfile(), closeFile);
    TEST_CHECK(file != nullptr);
    TEST_CHECK(std::fwrite(keymapText.get(), 1, size, file.get()) == size);
    TEST_CHECK(std::fflush(file.get()) == 0);
    const int fd = dup(fileno(file.get()));
    TEST_CHECK(fd >= 0);
    WaylandSeat::handleKeyboardKeymap(&seat, nullptr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
  }

  void key(WaylandSeat& seat, std::uint32_t code, bool pressed = true) {
    WaylandSeat::handleKeyboardKey(
        &seat, nullptr, 1, 0, code, pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
    );
  }

  void rate(WaylandSeat& seat, int hz, int delay = 0) {
    WaylandSeat::handleKeyboardRepeatInfo(&seat, nullptr, hz, delay);
  }

  void checkStopped(WaylandSeat& seat, const std::vector<KeyboardEvent>& events) {
    const auto count = events.size();
    TEST_CHECK(seat.repeatPollTimeoutMs() == -1);
    seat.repeatTick();
    TEST_CHECK(events.size() == count);
  }

  void testRepeat(WaylandSeat& seat, const std::vector<KeyboardEvent>& events) {
    loadKeymap(seat, "");
    rate(seat, 30, -100);
    key(seat, KEY_LEFTSHIFT);
    key(seat, KEY_CAPSLOCK);
    checkStopped(seat, events);
    key(seat, KEY_A);
    TEST_CHECK(!events.back().repeat && events.back().utf32 == 'a');
    key(seat, KEY_CAPSLOCK, false);
    key(seat, KEY_LEFTSHIFT);
    TEST_CHECK(seat.repeatPollTimeoutMs() == 0);

    WaylandSeat::handleKeyboardModifiers(&seat, nullptr, 1, 1, 0, 0, 0); // Shift in the US keymap.
    seat.repeatTick();
    TEST_CHECK(events.back().repeat && events.back().pressed && events.back().key == KEY_A);
    TEST_CHECK(events.back().sym == XKB_KEY_A && events.back().utf32 == 'A');
    TEST_CHECK(events.back().modifiers == KeyMod::Shift);
    const int nextTick = seat.repeatPollTimeoutMs();
    TEST_CHECK(nextTick >= 0 && nextTick <= 34);
    key(seat, KEY_LEFTSHIFT, false);
    TEST_CHECK(seat.repeatPollTimeoutMs() >= 0);
    key(seat, KEY_A, false);
    TEST_CHECK(!events.back().repeat && !events.back().pressed);
    checkStopped(seat, events);
    TEST_CHECK(seat.lastInputSource() == WaylandSeat::InputSource::Keyboard);
    WaylandSeat::handlePointerMotion(&seat, nullptr, 0, 0, 0);
    TEST_CHECK(seat.lastInputSource() == WaylandSeat::InputSource::Pointer);

    rate(seat, 30, 500);
    key(seat, KEY_B);
    TEST_CHECK(seat.repeatPollTimeoutMs() > 0);
    const auto count = events.size();
    seat.repeatTick();
    TEST_CHECK(events.size() == count);
    loadKeymap(seat, "intl");
    checkStopped(seat, events);
  }

  void testCompose(WaylandSeat& seat, const std::vector<KeyboardEvent>& events) {
    rate(seat, 30);
    key(seat, KEY_B);
    key(seat, KEY_APOSTROPHE);
    TEST_CHECK(events.back().preedit && events.back().sym == XKB_KEY_dead_acute);
    checkStopped(seat, events);
    key(seat, KEY_E);
    TEST_CHECK(!events.back().preedit && events.back().utf32 == U'é');
    seat.repeatTick();
    TEST_CHECK(events.back().repeat && events.back().utf32 == U'é');

    // A surface identity is sufficient for these callbacks. They never dereference it.
    int surfaceIdentity = 0;
    auto* surface = reinterpret_cast<wl_surface*>(&surfaceIdentity);
    WaylandSeat::handleKeyboardEnter(&seat, nullptr, 1, surface, nullptr);
    checkStopped(seat, events);
    key(seat, KEY_APOSTROPHE);
    WaylandSeat::handleKeyboardLeave(&seat, nullptr, 2, surface);
    checkStopped(seat, events);
    key(seat, KEY_E);
    TEST_CHECK(events.back().utf32 == 'e');

    WaylandSeat::handleKeyboardEnter(&seat, nullptr, 3, surface, nullptr);
    key(seat, KEY_APOSTROPHE);
    seat.forgetSurface(surface);
    checkStopped(seat, events);
    key(seat, KEY_E);
    TEST_CHECK(events.back().utf32 == 'e');
    WaylandSeat::handleKeyboardEnter(&seat, nullptr, 4, surface, nullptr);
    key(seat, KEY_E);
    TEST_CHECK(seat.repeatPollTimeoutMs() == 0);
    seat.forgetSurface(surface);
    checkStopped(seat, events);

    key(seat, KEY_APOSTROPHE);
    rate(seat, 0);
    checkStopped(seat, events);
    key(seat, KEY_E);
    TEST_CHECK(events.back().utf32 == 'e');
    checkStopped(seat, events);

    rate(seat, 30);
    key(seat, KEY_A);
    rate(seat, -1);
    checkStopped(seat, events);
  }

  void testCapabilityLoss(WaylandSeat& seat, const std::vector<KeyboardEvent>& events) {
    // Allocate real client proxies on a private socket without contacting a compositor.
    int sockets[2]{};
    TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    const std::unique_ptr<wl_display, decltype(&wl_display_disconnect)> display(
        wl_display_connect_to_fd(sockets[0]), wl_display_disconnect
    );
    close(sockets[1]);
    TEST_CHECK(display != nullptr);
    const std::unique_ptr<wl_registry, decltype(&wl_registry_destroy)> registry(
        wl_display_get_registry(display.get()), wl_registry_destroy
    );
    const std::unique_ptr<wl_seat, decltype(&wl_seat_destroy)> proxy(
        static_cast<wl_seat*>(wl_registry_bind(registry.get(), 1, &wl_seat_interface, 1)), wl_seat_destroy
    );
    TEST_CHECK(proxy != nullptr);
    WaylandSeat::handleSeatCapabilities(&seat, proxy.get(), WL_SEAT_CAPABILITY_KEYBOARD);
    rate(seat, 30);
    key(seat, KEY_E);
    const int timeout = seat.repeatPollTimeoutMs();
    WaylandSeat::handleSeatCapabilities(&seat, proxy.get(), 0);
    TEST_CHECK(timeout == 0);
    checkStopped(seat, events);
  }

} // namespace

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-seat-keyboard"});
    const auto compose = env.work() / "Compose";
    TEST_CHECK(static_cast<bool>(std::ofstream(compose) << "<dead_acute> <e> : \"é\" eacute\n"));
    nythera::test::ScopedEnvVar composeFile("XCOMPOSEFILE", compose.string());
    std::vector<KeyboardEvent> events;
    WaylandSeat seat;
    struct SeatCleanup {
      WaylandSeat& seat;
      ~SeatCleanup() { seat.cleanup(); }
    } cleanup{seat};
    seat.setKeyboardEventCallback([&](const KeyboardEvent& event) { events.push_back(event); });
    testRepeat(seat, events);
    testCompose(seat, events);
    testCapabilityLoss(seat, events);
  });
}
