#include "filemanager/property_scan_presentation.h"

#include <cassert>

int main() {
  using namespace filemanager;
  const FolderSizeResult complete{};
  const auto full = propertyScanPresentation(complete);
  assert(!full.showDisclosure && full.status.empty() && full.details.empty());

  FolderSizeResult permission;
  permission.permissionErrors = permission.errors = 1;
  permission.diagnostics.push_back({"locked", "open directory", 13, true, "Permission denied"});
  const auto denied = propertyScanPresentation(permission);
  assert(denied.showDisclosure && denied.status == "Partial.");
  assert(denied.details == "locked — open directory: Permission denied");

  FolderSizeResult mount;
  mount.excludedMounts = 1;
  assert(propertyScanPresentation(mount).showDisclosure);

  FolderSizeResult failed;
  failed.failed = true;
  failed.reason = "Root could not be read";
  const auto failure = propertyScanPresentation(failed);
  assert(failure.status == "Failed." && failure.details == "Root could not be read");

  FolderSizeResult cancelled;
  cancelled.cancelled = true;
  const auto stopped = propertyScanPresentation(cancelled);
  assert(stopped.status == "Cancelled." && stopped.status != failure.status);

  FolderSizeResult sampled;
  sampled.errors = 2;
  sampled.diagnostics = {{"", "open root", 5, true, "I/O error"}, {"nested/item", "stat", 2, false, "No such file"}};
  const auto details = propertyScanPresentation(sampled);
  assert(details.details == ". — open root: I/O error\nnested/item — stat: No such file");
  assert(details.showDisclosure && !details.details.empty());

  const auto root = propertyNamePresentation(true, "");
  assert(root.text == "/" && !root.editable);
  const auto ordinary = propertyNamePresentation(false, "notes");
  assert(ordinary.text == "notes" && ordinary.editable);
}
