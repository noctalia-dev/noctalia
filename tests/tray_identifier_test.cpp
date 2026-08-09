#include "shell/tray/tray_identifier.h"

#include <cassert>

int main() {
  TrayItemInfo item;

  item.status = "Passive";
  assert(tray::isPassiveStatus(item));

  item.status = "passive";
  assert(tray::isPassiveStatus(item));

  item.status = "Active";
  assert(!tray::isPassiveStatus(item));

  item.status = "NeedsAttention";
  assert(!tray::isPassiveStatus(item));

  item.status.clear();
  assert(!tray::isPassiveStatus(item));
}
