// Quitting must not wait out a template hook that never exits, and the whole teardown must
// cost one shutdown grace, not one per object in the teardown chain.

#include "config/config_service.h"
#include "tests/test_check.h"
#include "theme/palette.h"
#include "theme/template_apply_service.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

  using noctalia::theme::GeneratedPalette;
  using noctalia::theme::TemplateApplyService;

  GeneratedPalette darkPalette() {
    GeneratedPalette palette;
    palette.dark["mSurface"] = 0x111111;
    palette.light["mSurface"] = 0x111111;
    return palette;
  }

  void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
  }

  const std::filesystem::path& sandboxBase() {
    static const std::filesystem::path base =
        std::filesystem::temp_directory_path() / ("noctalia-template-shutdown-" + std::to_string(::getpid()));
    return base;
  }

  // The assets directory is resolved once per process, so every scenario shares it and swaps
  // the built-in template config in place. Config and state homes are re-read per scenario.
  std::filesystem::path assetsDir() { return sandboxBase() / "assets"; }

  void makeAssets() {
    std::filesystem::remove_all(sandboxBase());
    std::filesystem::create_directories(assetsDir() / "templates");
    std::filesystem::create_directories(assetsDir() / "fonts");
    std::filesystem::create_directories(assetsDir() / "translations");
    // A minimal asset bundle: only the built-in template config is read by these tests.
    writeFile(assetsDir() / "emoji.json", "{}\n");
    writeFile(assetsDir() / "fonts" / "noctalia-tabler.ttf", "");
    writeFile(assetsDir() / "translations" / "en.json", "{}\n");
    writeFile(assetsDir() / "templates" / "input.txt", "surface\n");
    ::setenv("NOCTALIA_ASSETS_DIR", assetsDir().c_str(), 1);
  }

  // A private config/state tree per scenario, so a worker left behind by an earlier one
  // cannot reach into the next.
  std::filesystem::path makeSandbox(const std::string& name) {
    const std::filesystem::path root = sandboxBase() / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "config" / "noctalia");
    std::filesystem::create_directories(root / "state" / "noctalia");
    std::filesystem::create_directories(root / "data");
    ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
    ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
    return root;
  }

  bool waitForFile(const std::filesystem::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(path) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::filesystem::exists(path);
  }

  // The worker is parked in a built-in undo hook that ignores SIGTERM, which is the hook path
  // a shutdown request cannot skip. Destruction must still return within its grace bounds.
  void stuckSyncHookDoesNotHoldUpShutdown() {
    const auto root = makeSandbox("sync");
    const auto started = root / "started";
    writeFile(
        assetsDir() / "templates" / "builtin.toml",
        "[templates.stuck]\nundo_hook = 'trap \"\" TERM; printf ran > " + started.string() + "; sleep 5'\n"
    );
    // The built-in template is no longer enabled, so applying owes its undo hook.
    writeFile(root / "config" / "noctalia" / "config.toml", "[theme.templates]\nenable_builtin_templates = false\n");
    writeFile(root / "state" / "noctalia" / "state.toml", "[theme_templates]\napplied_builtin_ids = 'stuck'\n");

    ConfigService config;
    auto service = std::make_unique<TemplateApplyService>(config, std::chrono::milliseconds(100));
    service->apply(darkPalette(), "dark", /*force=*/false, /*paletteChanged=*/true);
    TEST_CHECK(waitForFile(started));

    const auto start = std::chrono::steady_clock::now();
    service.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    TEST_CHECK(elapsed < std::chrono::seconds(2));

    std::filesystem::remove_all(root);
  }

  // A stuck synchronous hook and a still-running asynchronous hook at once: the service and its
  // hook runner must share one shutdown budget instead of each spending a grace period of its own.
  void bothHookPathsShareOneShutdownBudget() {
    const auto root = makeSandbox("both");
    const auto asyncStarted = root / "async-started";
    const auto syncStarted = root / "sync-started";
    writeFile(
        assetsDir() / "templates" / "builtin.toml",
        // No output_path, so the post hook runs on its own, asynchronously, through the runner.
        "[templates.spinner]\nindex = 1\ninput_path = 'input.txt'\nhook_async = true\n"
        "post_hook = 'trap \"\" TERM; printf ran > "
            + asyncStarted.string()
            + "; sleep 30'\n"
              // An output_path makes the pre hook run, synchronously, on the worker thread.
              "\n[templates.blocker]\nindex = 2\ninput_path = 'input.txt'\noutput_path = '"
            + (root / "out.txt").string()
            + "'\n"
              "pre_hook = 'trap \"\" TERM; printf ran > "
            + syncStarted.string()
            + "; sleep 30'\n"
    );
    writeFile(
        root / "config" / "noctalia" / "config.toml",
        "[theme.templates]\nenable_builtin_templates = true\nbuiltin_ids = ['spinner', 'blocker']\n"
    );
    writeFile(root / "state" / "noctalia" / "state.toml", "[theme_templates]\n");

    constexpr auto kGrace = std::chrono::milliseconds(1000);
    ConfigService config;
    auto service = std::make_unique<TemplateApplyService>(config, kGrace);
    service->apply(darkPalette(), "dark", /*force=*/false, /*paletteChanged=*/true);
    TEST_CHECK(waitForFile(asyncStarted));
    TEST_CHECK(waitForFile(syncStarted));

    const auto start = std::chrono::steady_clock::now();
    service.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // One grace plus the reap allowance. Two independent ladders would cost about twice this.
    TEST_CHECK(elapsed < kGrace * 9 / 5);

    std::filesystem::remove_all(root);
  }

  // The worker blocked outside any hook, where raising the cancel flag is a no-op: the template
  // output is a FIFO nobody reads, so reading its previous contents never returns. Only the
  // detach fallback can end this shutdown.
  void workerBlockedBeyondCancelIsLeftBehind() {
    const auto root = makeSandbox("detach");
    const auto fifo = root / "fifo";
    TEST_CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    writeFile(
        assetsDir() / "templates" / "builtin.toml",
        "[templates.fifo]\nindex = 1\ninput_path = 'input.txt'\noutput_path = '" + fifo.string() + "'\n"
    );
    writeFile(
        root / "config" / "noctalia" / "config.toml",
        "[theme.templates]\nenable_builtin_templates = true\nbuiltin_ids = ['fifo']\n"
    );
    writeFile(root / "state" / "noctalia" / "state.toml", "[theme_templates]\n");

    constexpr auto kGrace = std::chrono::milliseconds(500);
    ConfigService config;
    auto service = std::make_unique<TemplateApplyService>(config, kGrace);
    service->apply(darkPalette(), "dark", /*force=*/false, /*paletteChanged=*/true);
    // The worker opens the fifo for reading and never comes back; give it time to get there.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    const auto start = std::chrono::steady_clock::now();
    service.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    TEST_CHECK(elapsed >= kGrace);
    TEST_CHECK(elapsed < std::chrono::seconds(3));

    // The worker is still parked on the fifo and owns the shared state; leave the tree in place.
  }

} // namespace

int main() {
  makeAssets();
  stuckSyncHookDoesNotHoldUpShutdown();
  bothHookPathsShareOneShutdownBudget();
  workerBlockedBeyondCancelIsLeftBehind();

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  ::unsetenv("NOCTALIA_ASSETS_DIR");
  return 0;
}
