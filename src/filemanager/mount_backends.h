#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace filemanager {
  enum class MountBackend { None, UDisks, Systemd };
  struct MountRecord {
    std::filesystem::path path;
    std::string source;
    std::string type;
    std::string root;
    bool systemCritical = false;
  };
  struct ManagedMount {
    MountBackend backend = MountBackend::None;
    MountRecord mount;
    std::string object;
    std::string unit;
  };
  // Pure policy. UDisks operates on a filesystem, not an arbitrary subvolume.
  bool protectedMount(const std::filesystem::path& path);
  bool isolatedFilesystem(const MountRecord& mount, const std::vector<MountRecord>& mounts);
  bool safeMountUnit(GVariant* unit, GVariant* mount);
  std::vector<MountRecord> mountRecords();
  std::string mountBackendError(const GError* error);
  void discoverManagedMounts(std::function<void(std::vector<ManagedMount>)> completion);
  void operateManagedMount(
      const ManagedMount& mount, GCancellable* cancellable, std::function<void(std::string)> completion,
      std::function<std::vector<MountRecord>()> snapshot = mountRecords
  );
} // namespace filemanager
