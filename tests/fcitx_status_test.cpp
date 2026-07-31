#include "dbus/tray/fcitx_status.h"
#include "dbus/tray/tray_service.h"

#include <cassert>
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
    assert(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    auto unrelated = fcitxItem();
    unrelated.itemName = "Dropbox";
    unrelated.processName = "dropbox";
    const std::vector items{unrelated};
    assert(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    const std::vector items{fcitxItem()};
    const auto state = resolveFcitxInputMethodState(items);
    assert(state.has_value());
    assert(state->label == "Rime");
  }

  {
    auto english = fcitxItem();
    english.iconName = "rime-disable";
    english.statusNotifierTitle = "Keyboard - English (US)";
    const std::vector items{english};
    const auto state = resolveFcitxInputMethodState(items);
    assert(state.has_value());
    assert(state->label == "Keyboard - English (US)");
  }

  {
    auto byProcess = fcitxItem();
    byProcess.itemName.clear();
    const std::vector items{byProcess};
    const auto state = resolveFcitxInputMethodState(items);
    assert(state.has_value());
    assert(state->label == "Rime");
  }

  {
    auto incomplete = fcitxItem();
    incomplete.statusNotifierTitle.clear();
    const std::vector items{incomplete};
    assert(!resolveFcitxInputMethodState(items).has_value());
  }

  {
    FcitxInputMethodTracker tracker;
    const std::vector<TrayItemInfo> empty;
    assert(!tracker.update(empty).has_value());
    assert(!tracker.available());

    auto unrelated = fcitxItem();
    unrelated.itemName = "Dropbox";
    unrelated.processName = "dropbox";
    assert(!tracker.update(std::vector{unrelated}).has_value());
    assert(!tracker.available());

    const auto rime = fcitxItem();
    assert(!tracker.update(std::vector{rime}).has_value());
    assert(tracker.available());
    assert(!tracker.update(std::vector{unrelated, rime}).has_value());

    auto english = rime;
    english.iconName = "rime-disable";
    english.statusNotifierTitle = "Keyboard - English (US)";
    const auto changed = tracker.update(std::vector{english});
    assert(changed.has_value());
    assert(changed->label == "Keyboard - English (US)");
    assert(!tracker.update(std::vector{english}).has_value());

    assert(!tracker.update(empty).has_value());
    assert(!tracker.available());
    assert(!tracker.update(std::vector{rime}).has_value());
    assert(tracker.available());
  }
}
