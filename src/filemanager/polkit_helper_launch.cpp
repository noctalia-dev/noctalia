#include "filemanager/polkit_helper_launch.h"

#include <polkit/polkit.h>
#include <sys/stat.h>
#include <unistd.h>

namespace filemanager {

  bool verifyInstalledPolkitFile(const std::filesystem::path& path) noexcept {
    if (path.empty() || !path.is_absolute())
      return false;
    // Reject user-writable install prefixes as well as writable helper/policy files or symlinks.
    for (auto current = path; !current.empty();
         current = current == current.root_path() ? std::filesystem::path{} : current.parent_path()) {
      struct stat state{};
      if (lstat(current.c_str(), &state) != 0
          || state.st_uid != 0
          || (state.st_mode & 0022) != 0
          || S_ISLNK(state.st_mode)) {
        return false;
      }
      if (current == path ? !S_ISREG(state.st_mode) : !S_ISDIR(state.st_mode))
        return false;
    }
    return true;
  }

  bool verifyInstalledPolkitTarget(const PolkitHelperTarget& target, std::string& errorOut) {
    if (!verifyInstalledPolkitFile(target.helperPath) || !verifyInstalledPolkitFile(target.policyPath)) {
      errorOut = "The root-owned "
          + std::string(target.displayName)
          + " helper and Polkit policy must be installed before authorisation is available.";
      return false;
    }
    return true;
  }

  bool verifyRegisteredPolkitAction(
      const PolkitHelperTarget& target, GCancellable* cancellable, bool& cancelledOut, std::string& errorOut
  ) {
    cancelledOut = false;
    if (target.actionId.empty() || target.helperPath.empty()) {
      errorOut = "Invalid Polkit action or helper executable path.";
      return false;
    }

    // Never fall back to the generic org.freedesktop.policykit.exec action.
    GError* policyError = nullptr;
    PolkitAuthority* authority = polkit_authority_get_sync(cancellable, &policyError);
    GList* actions =
        authority != nullptr ? polkit_authority_enumerate_actions_sync(authority, cancellable, &policyError) : nullptr;
    bool dedicated = false;
    for (GList* item = actions; item != nullptr; item = item->next) {
      auto* action = POLKIT_ACTION_DESCRIPTION(item->data);
      const auto* execPath = polkit_action_description_get_annotation(action, "org.freedesktop.policykit.exec.path");
      const auto* id = polkit_action_description_get_action_id(action);
      if (id != nullptr
          && std::string_view(id) == target.actionId
          && execPath != nullptr
          && std::string_view(execPath) == target.helperPath) {
        dedicated = true;
        break;
      }
    }
    g_list_free_full(actions, g_object_unref);
    if (authority != nullptr)
      g_object_unref(authority);

    if (!dedicated) {
      cancelledOut = g_cancellable_is_cancelled(cancellable) != FALSE;
      errorOut = policyError != nullptr
          ? policyError->message
          : ("The dedicated " + std::string(target.displayName) + " Polkit action is not registered.");
      g_clear_error(&policyError);
      return false;
    }
    g_clear_error(&policyError);
    if (g_cancellable_is_cancelled(cancellable) != FALSE) {
      cancelledOut = true;
      return false;
    }
    return true;
  }

  bool verifyPolkitHelperReady(
      const PolkitHelperTarget& target, GCancellable* cancellable, bool& cancelledOut, std::string& errorOut
  ) {
    cancelledOut = false;
    if (!verifyInstalledPolkitTarget(target, errorOut))
      return false;
    return verifyRegisteredPolkitAction(target, cancellable, cancelledOut, errorOut);
  }

  GSubprocess* spawnPolkitHelperProcess(
      std::string_view helperPath, const std::vector<std::string>& helperArgs, std::string& errorOut
  ) {
    if (helperPath.empty()) {
      errorOut = "No helper executable path specified.";
      return nullptr;
    }
    std::vector<std::string> arguments;
    arguments.reserve(helperArgs.size() + 3);
    arguments.emplace_back("/usr/bin/pkexec");
    arguments.emplace_back("--disable-internal-agent");
    arguments.emplace_back(helperPath);
    for (const auto& arg : helperArgs)
      arguments.push_back(arg);

    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& arg : arguments)
      argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    GError* error = nullptr;
    auto* process = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE
        ),
        &error
    );
    if (process == nullptr) {
      errorOut = error != nullptr ? error->message : "Could not start authorisation.";
      g_clear_error(&error);
      return nullptr;
    }
    g_clear_error(&error);
    return process;
  }

  PolkitAuthFailure classifyPolkitExitStatus(int exitCode, bool cancelled) {
    PolkitAuthFailure failure;
    failure.cancelled = cancelled || exitCode == 126;
    failure.denied = !failure.cancelled && exitCode == 127;
    if (failure.cancelled)
      failure.error = "Authorisation cancelled; no new changes will be started.";
    else if (failure.denied)
      failure.error = "Authorisation was denied or unavailable.";
    else
      failure.error = "The authorised helper failed or returned no result.";
    return failure;
  }

} // namespace filemanager
