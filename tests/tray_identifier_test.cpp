#include "shell/tray/tray_identifier.h"
#include "tests/test_check.h"

int main() {
  TrayItemInfo item;

  item.status = "Passive";
  TEST_CHECK(tray::isPassiveStatus(item));

  item.status = "passive";
  TEST_CHECK(tray::isPassiveStatus(item));

  item.status = "Active";
  TEST_CHECK(!tray::isPassiveStatus(item));

  item.status = "NeedsAttention";
  TEST_CHECK(!tray::isPassiveStatus(item));

  item.status.clear();
  TEST_CHECK(!tray::isPassiveStatus(item));
}
