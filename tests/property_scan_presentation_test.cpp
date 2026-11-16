#include "filemanager/property_scan_presentation.h"

#include <cassert>

int main() {
  using namespace filemanager;
  const FolderSizeResult complete{};
  const auto full = propertyScanPresentation(complete);
  assert(!full.showDisclosure && full.summary.empty() && full.details.empty());

  FolderSizeResult permission;
  permission.permissionErrors = permission.errors = 1;
  permission.diagnostics.push_back({"locked", "open directory", 13, true, "Permission denied"});
  const auto denied = propertyScanPresentation(permission);
  assert(denied.showDisclosure && denied.summary == "1 inaccessible path");
  assert(denied.details.size() == 2);
  assert((denied.details[0] == std::pair<std::string, std::string>{"Permission failures", "1"}));
  assert((denied.details[1] == std::pair<std::string, std::string>{
      "Path 1", "locked\nopen directory: Permission denied\nDescendants may be omitted"}));

  FolderSizeResult mount;
  mount.excludedMounts = 1;
  const auto excluded = propertyScanPresentation(mount);
  assert(excluded.showDisclosure && excluded.summary == "1 child mount excluded");

  FolderSizeResult failed;
  failed.failed = true;
  failed.reason = "Root could not be read";
  const auto failure = propertyScanPresentation(failed);
  assert(failure.summary == "Scan failed");
  assert((failure.details.front() == std::pair<std::string, std::string>{"Root error", "Root could not be read"}));

  FolderSizeResult cancelled;
  cancelled.cancelled = true;
  const auto stopped = propertyScanPresentation(cancelled);
  assert(stopped.summary == "Scan cancelled" && stopped.summary != failure.summary);

  FolderSizeResult sampled;
  sampled.errors = 2;
  sampled.diagnostics = {{"", "open root", 5, true, "I/O error"}, {"nested/item", "stat", 2, false, "No such file"}};
  const auto details = propertyScanPresentation(sampled);
  assert(details.summary == "2 other scan errors");
  assert(details.details.size() == 3);
  assert((details.details[0] == std::pair<std::string, std::string>{"Other scan errors", "2"}));
  assert((details.details[1] == std::pair<std::string, std::string>{
      "Path 1", ".\nopen root: I/O error\nDescendants may be omitted"}));
  assert((details.details[2] == std::pair<std::string, std::string>{"Path 2", "nested/item\nstat: No such file"}));

  FolderSizeResult mixed;
  mixed.errors = mixed.permissionErrors = 3;
  mixed.excludedMounts = 2;
  const auto summary = propertyScanPresentation(mixed);
  assert(summary.summary == "3 inaccessible paths · 2 child mounts excluded");
  assert((summary.details[0] == std::pair<std::string, std::string>{"Permission failures", propertyScanCountText(3)}));
  assert((summary.details[1] == std::pair<std::string, std::string>{"Excluded child mounts", propertyScanCountText(2)}));

  const auto root = propertyNamePresentation(true, "");
  assert(root.text == "/" && !root.editable);
  const auto ordinary = propertyNamePresentation(false, "notes");
  assert(ordinary.text == "notes" && ordinary.editable);
}
