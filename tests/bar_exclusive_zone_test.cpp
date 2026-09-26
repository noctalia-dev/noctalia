#include "shell/bar/bar_reserved_zone.h"

#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  BarConfig autoHideBar() {
    BarConfig cfg;
    cfg.reserveSpace = true;
    cfg.autoHide = true;
    return cfg;
  }

  BarConfig smartHideBar() {
    BarConfig cfg;
    cfg.reserveSpace = true;
    cfg.smartAutoHide = true;
    return cfg;
  }

} // namespace

int main() {
  bool ok = true;

  BarConfig alwaysOn;
  alwaysOn.reserveSpace = true;
  ok &= check(barShouldReserveExclusiveZone(alwaysOn, false, false), "always-on bar reserves even when not shown");
  ok &= check(barShouldReserveExclusiveZone(alwaysOn, false, true), "always-on bar reserves when shown");

  auto autoHide = autoHideBar();
  ok &= check(barShouldReserveExclusiveZone(autoHide, false, false), "auto-hide hidden still reserves space by default");
  ok &= check(barShouldReserveExclusiveZone(autoHide, false, true), "auto-hide shown still reserves space by default");
  ok &= check(!barShouldReserveExclusiveZone(autoHide, true, true), "ipc hide releases reserve space");

  autoHide.autoHideReserveSpace = true;
  ok &= check(
      !barShouldReserveExclusiveZone(autoHide, false, false),
      "auto-hide with reserve-while-visible releases space when hidden"
  );
  ok &= check(
      barShouldReserveExclusiveZone(autoHide, false, true),
      "auto-hide with reserve-while-visible reserves space when shown"
  );

  auto smartHide = smartHideBar();
  ok &= check(
      barShouldReserveExclusiveZone(smartHide, false, false), "smart auto-hide hidden keeps static reserve by default"
  );
  ok &= check(barShouldReserveExclusiveZone(smartHide, false, true), "smart auto-hide shown reserves space by default");

  smartHide.autoHideReserveSpace = true;
  ok &= check(
      !barShouldReserveExclusiveZone(smartHide, false, false),
      "smart auto-hide with reserve-while-visible releases space when hidden"
  );
  ok &= check(
      barShouldReserveExclusiveZone(smartHide, false, true),
      "smart auto-hide with reserve-while-visible reserves space when shown"
  );

  BarConfig noReserve = smartHideBar();
  noReserve.reserveSpace = false;
  ok &= check(!barShouldReserveExclusiveZone(noReserve, false, true), "reserve_space off never reserves");

  return ok ? 0 : 1;
}
