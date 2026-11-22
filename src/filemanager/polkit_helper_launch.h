#pragma once

#include <filesystem>
#include <gio/gio.h>
#include <string>
#include <string_view>
#include <vector>

namespace filemanager {

  struct PolkitHelperTarget {
    std::string_view helperPath;
    std::string_view policyPath;
    std::string_view actionId;
    std::string_view displayName;
  };

  struct PolkitAuthFailure {
    bool cancelled = false;
    bool denied = false;
    std::string error;
  };

  // Verifies that `path` is an absolute path to a regular root-owned file (not a symlink,
  // not group/world-writable) and that every ancestor directory up to `/` is a root-owned,
  // non-symlink, non-group/world-writable directory.
  [[nodiscard]] bool verifyInstalledPolkitFile(const std::filesystem::path& path) noexcept;

  // Verifies both the helper binary and the Polkit policy file on disk.
  [[nodiscard]] bool verifyInstalledPolkitTarget(const PolkitHelperTarget& target, std::string& errorOut);

  // Verifies with PolkitAuthority that `target.actionId` is registered and explicitly
  // annotated with `org.freedesktop.policykit.exec.path == target.helperPath`.
  // Never falls back to generic `org.freedesktop.policykit.exec`.
  [[nodiscard]] bool verifyRegisteredPolkitAction(
      const PolkitHelperTarget& target, GCancellable* cancellable, bool& cancelledOut, std::string& errorOut
  );

  // Verifies installation paths and dedicated Polkit action registration in sequence.
  [[nodiscard]] bool verifyPolkitHelperReady(
      const PolkitHelperTarget& target, GCancellable* cancellable, bool& cancelledOut, std::string& errorOut
  );

  // Spawns `/usr/bin/pkexec --disable-internal-agent <helperPath> [args...]` with private
  // stdin/stdout pipes and silenced stderr.
  [[nodiscard]] GSubprocess* spawnPolkitHelperProcess(
      std::string_view helperPath, const std::vector<std::string>& helperArgs, std::string& errorOut
  );

  // Classifies pkexec exit codes (126 = dismissed/cancelled, 127 = denied/not authorized).
  [[nodiscard]] PolkitAuthFailure classifyPolkitExitStatus(int exitCode, bool cancelled);

} // namespace filemanager
