#include "dbus/tray/fcitx_status.h"
#include "dbus/tray/tray_service.h"
#include "tests/test_check.h"

#include <vector>

namespace {

  TrayItemInfo fcitxItem() {
    return TrayItemInfo{
        .id = ":1.42/StatusNotifierItem",
        .busName = ":1.42",
        .objectPath = "/StatusNotifierItem",
        .iconName = "fcitx-rime",
        .itemName = "Fcitx",
        .processName = "fcitx5",
        .title = "Input Method",
        .statusNotifierTitle = "Rime",
        .status = "Active",
    };
  }

} // namespace

int main() {
  {
    const std::vector<TrayItemInfo> items;
    TEST_CHECK(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    auto unrelated = fcitxItem();
    unrelated.itemName = "Dropbox";
    unrelated.processName = "dropbox";
    const std::vector items{unrelated};
    TEST_CHECK(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    const std::vector items{fcitxItem()};
    const auto state = resolveFcitxInputMethodState(items);
    TEST_CHECK(state.has_value());
    TEST_CHECK(state->label == "Rime");
  }

  {
    auto english = fcitxItem();
    english.iconName = "rime-disable";
    english.statusNotifierTitle = "Keyboard - English (US)";
    const std::vector items{english};
    const auto state = resolveFcitxInputMethodState(items);
    TEST_CHECK(state.has_value());
    TEST_CHECK(state->label == "Keyboard - English (US)");
  }

  {
    auto byProcess = fcitxItem();
    byProcess.itemName.clear();
    const std::vector items{byProcess};
    const auto state = resolveFcitxInputMethodState(items);
    TEST_CHECK(state.has_value());
    TEST_CHECK(state->label == "Rime");
  }

  {
    auto incomplete = fcitxItem();
    incomplete.statusNotifierTitle.clear();
    const std::vector items{incomplete};
    TEST_CHECK(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    FcitxInputMethodTracker tracker;
    const std::vector<TrayItemInfo> empty;
    TEST_CHECK(!tracker.update(empty).has_value());
    TEST_CHECK(!tracker.available());

    auto unrelated = fcitxItem();
    unrelated.itemName = "Dropbox";
    unrelated.processName = "dropbox";
    TEST_CHECK(!tracker.update(std::vector{unrelated}).has_value());
    TEST_CHECK(!tracker.available());

    const auto rime = fcitxItem();
    TEST_CHECK(!tracker.update(std::vector{rime}).has_value());
    TEST_CHECK(tracker.available());
    TEST_CHECK(!tracker.update(std::vector{unrelated, rime}).has_value());

    auto english = rime;
    english.iconName = "rime-disable";
    english.statusNotifierTitle = "Keyboard - English (US)";
    const auto changed = tracker.update(std::vector{english});
    TEST_CHECK(changed.has_value());
    TEST_CHECK(changed->label == "Keyboard - English (US)");
    TEST_CHECK(!tracker.update(std::vector{english}).has_value());

    TEST_CHECK(!tracker.update(empty).has_value());
    TEST_CHECK(!tracker.available());
    TEST_CHECK(!tracker.update(std::vector{rime}).has_value());
    TEST_CHECK(tracker.available());
  }
}
