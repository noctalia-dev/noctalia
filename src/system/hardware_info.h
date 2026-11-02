#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

std::string cpuModelName();
std::string gpuLabel();
std::string motherboardLabel();
std::string memoryTotalLabel();
std::string compositorLabel();

// First non-blank line of the compositor's version output with its first letter capitalized (e.g.
// "Umbriel 0.1.0 (350794892e2b)"). Probed once per process on a worker thread. Main thread only. Returns nullopt while
// unresolved; `onResolved` then runs on the main loop once the probe finishes (only the latest pending
// callback is kept). Resolves to an empty string when the compositor is unknown or printed nothing.
std::optional<std::string> compositorVersionLine(std::function<void()> onResolved);

// Mount points of physical (block-device backed) filesystems, deduped by backing device and sorted
// so the root filesystem comes first. Excludes pseudo filesystems, snap/loop images, boot and EFI.
std::vector<std::string> physicalDiskMountPoints();

// "total GB (NN%)" for the filesystem containing the given mount point.
std::string diskUsageLabel(const std::string& mountPoint);
