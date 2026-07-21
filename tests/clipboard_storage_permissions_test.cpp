#include "wayland/clipboard_service.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <print>
#include <string>

namespace {
  constexpr std::filesystem::perms permissionMask() {
    using P = std::filesystem::perms;
    return P::owner_read
        | P::owner_write
        | P::owner_exec
        | P::group_read
        | P::group_write
        | P::group_exec
        | P::others_read
        | P::others_write
        | P::others_exec;
  }

  constexpr std::filesystem::perms privateFileMode() {
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  }

  constexpr std::filesystem::perms privateDirectoryMode() {
    return privateFileMode() | std::filesystem::perms::owner_exec;
  }

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "clipboard_storage_permissions_test: {}", message);
    }
    return condition;
  }

  std::filesystem::perms mode(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::status(path, ec).permissions() & permissionMask();
  }

  void makeWorldReadable(const std::filesystem::path& path, bool directory) {
    using P = std::filesystem::perms;
    const P requested = directory ? P::owner_all | P::group_read | P::group_exec | P::others_read | P::others_exec
                                  : privateFileMode() | P::group_read | P::others_read;
    std::filesystem::permissions(path, requested, std::filesystem::perm_options::replace);
  }
} // namespace

int main() {
  namespace fs = std::filesystem;

  const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path stateHome = fs::temp_directory_path() / ("noctalia-clipboard-permissions-test-" + std::to_string(serial));
  const fs::path clipboardDir = stateHome / "noctalia/clipboard";
  const fs::path entriesDir = clipboardDir / "entries";
  const fs::path manifestPath = clipboardDir / "index.json";
  const fs::path payloadPath = entriesDir / "entry.bin";

  fs::remove_all(stateHome);
  fs::create_directories(entriesDir);
  {
    std::ofstream payload(payloadPath, std::ios::binary | std::ios::trunc);
    payload << "secret";
  }
  {
    std::ofstream manifest(manifestPath, std::ios::trunc);
    manifest << nlohmann::json{
        {"entries",
         nlohmann::json::array({
             {
                 {"id", "entry"},
                 {"payload_path", payloadPath.string()},
                 {"mime_types", std::vector<std::string>{"text/plain"}},
                 {"data_mime_type", "text/plain"},
                 {"text_preview", "secret"},
                 {"byte_size", 6},
                 {"captured_at_ms", 0},
                 {"pinned", false},
             },
         })}}
             .dump(2);
  }
  makeWorldReadable(clipboardDir, true);
  makeWorldReadable(entriesDir, true);
  makeWorldReadable(manifestPath, false);
  makeWorldReadable(payloadPath, false);

  bool ok = true;
  ok = expect(::setenv("NOCTALIA_STATE_HOME", stateHome.c_str(), 1) == 0, "failed to set test state directory") && ok;
  {
    ClipboardService clipboard;
    ok = expect(mode(clipboardDir) == privateDirectoryMode(), "existing clipboard directory mode was not 0700") && ok;
    ok = expect(mode(entriesDir) == privateDirectoryMode(), "existing entries directory mode was not 0700") && ok;
    ok = expect(mode(manifestPath) == privateFileMode(), "existing manifest mode was not tightened to 0600") && ok;
    ok = expect(mode(payloadPath) == privateFileMode(), "existing payload mode was not tightened to 0600") && ok;
    ok = expect(clipboard.ensureEntryLoaded(0), "failed to load seeded clipboard payload") && ok;

    fs::remove(manifestPath);
    fs::remove(payloadPath);
    ok = expect(clipboard.setEntryPinned(0, true), "failed to persist seeded clipboard entry") && ok;
    ok = expect(mode(manifestPath) == privateFileMode(), "new manifest mode was not 0600") && ok;
    ok = expect(mode(payloadPath) == privateFileMode(), "new payload mode was not 0600") && ok;
  }

  ::unsetenv("NOCTALIA_STATE_HOME");
  fs::remove_all(stateHome);
  return ok ? 0 : 1;
}
