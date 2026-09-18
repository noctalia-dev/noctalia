// Destroying TemplateApplyService must not hang on a template hook that never exits. Every
// hook here ignores SIGTERM, so only the SIGKILL escalation ends it; the test checks that
// teardown returns within one grace period and leaves no hook process behind.

#include "config/config_service.h"
#include "tests/test_check.h"
#include "theme/palette.h"
#include "theme/template_apply_service.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  using Clock = std::chrono::steady_clock;
  using noctalia::theme::GeneratedPalette;
  using noctalia::theme::TemplateApplyService;

  constexpr std::chrono::milliseconds kGrace{1500};

  void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
  }

  pid_t waitForPid(const std::filesystem::path& path) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
      std::ifstream in(path);
      std::string contents;
      if (std::getline(in, contents) && !contents.empty()) {
        return static_cast<pid_t>(std::stol(contents));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
  }

  bool processGone(pid_t pid) { return ::kill(pid, 0) != 0 && errno == ESRCH; }

  std::string hangCommand(const std::filesystem::path& pidFile) {
    return "trap '' TERM; echo $$ > " + pidFile.string() + "; exec sleep 600";
  }

  GeneratedPalette palette() {
    GeneratedPalette result;
    result.dark["mSurface"] = 0x111111;
    result.light["mSurface"] = 0x111111;
    return result;
  }

  // Applies `templates` (the body of [theme.templates.user.*] tables), waits until every
  // pid file is written, then destroys the service and returns how long that took.
  double applyThenDestroy(
      const std::filesystem::path& root, const std::string& templates,
      const std::vector<std::filesystem::path>& pidFiles, std::vector<pid_t>& pids
  ) {
    writeFile(root / "config" / "noctalia" / "config.toml", templates);

    auto config = std::make_unique<ConfigService>();
    auto service = std::make_unique<TemplateApplyService>(*config, kGrace);
    service->apply(palette(), "dark", /*force=*/true, /*paletteChanged=*/true);

    pids.clear();
    for (const auto& pidFile : pidFiles) {
      pids.push_back(waitForPid(pidFile));
    }

    const auto start = Clock::now();
    service.reset();
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    config.reset();
    return elapsed;
  }

  // The issue's reproduction: a synchronous post_hook blocks the worker, so the join in the
  // destructor used to wait for it forever.
  void test_stuck_inline_post_hook(const std::filesystem::path& root) {
    const auto pidFile = root / "inline.pid";
    const std::string templates = "[theme.templates.user.inline]\n"
                                  "enabled = true\n"
                                  "input_path = \""
        + (root / "input.tmpl").string()
        + "\"\n"
          "output_path = \""
        + (root / "inline.out").string()
        + "\"\n"
          "post_hook = \""
        + hangCommand(pidFile)
        + "\"\n"
          "hook_async = false\n";

    std::vector<pid_t> pids;
    const double elapsed = applyThenDestroy(root, templates, {pidFile}, pids);

    TEST_CHECK(pids[0] > 0);
    TEST_CHECK(elapsed < 3.0);
    TEST_CHECK(processGone(pids[0]));
  }

  // A pre_hook is not a barrier, so it can block the worker while an asynchronous post_hook
  // from an earlier template is still running. Both must be terminated on the same deadline;
  // waiting one grace for each would take at least twice as long.
  void test_stuck_sync_and_async_hooks_share_one_grace(const std::filesystem::path& root) {
    const auto asyncPid = root / "async.pid";
    const auto syncPid = root / "sync.pid";
    const std::string input = (root / "input.tmpl").string();
    const std::string templates = "[theme.templates.user.first]\n"
                                  "enabled = true\n"
                                  "index = 1\n"
                                  "input_path = \""
        + input
        + "\"\n"
          "output_path = \""
        + (root / "first.out").string()
        + "\"\n"
          "post_hook = \""
        + hangCommand(asyncPid)
        + "\"\n"
          "\n"
          "[theme.templates.user.second]\n"
          "enabled = true\n"
          "index = 2\n"
          "input_path = \""
        + input
        + "\"\n"
          "output_path = \""
        + (root / "second.out").string()
        + "\"\n"
          "pre_hook = \""
        + hangCommand(syncPid)
        + "\"\n";

    std::vector<pid_t> pids;
    const double elapsed = applyThenDestroy(root, templates, {asyncPid, syncPid}, pids);

    TEST_CHECK(pids[0] > 0);
    TEST_CHECK(pids[1] > 0);
    TEST_CHECK(elapsed < 3.0);
    TEST_CHECK(processGone(pids[0]));
    TEST_CHECK(processGone(pids[1]));
  }

  // A built-in template that was applied and is now disabled owes its undo_hook, which the
  // worker runs synchronously before rendering anything. It must be bounded the same way.
  void test_stuck_undo_hook(const std::filesystem::path& root) {
    const auto pidFile = root / "undo.pid";
    writeFile(
        root / "assets" / "templates" / "builtin.toml",
        "[templates.stuck]\nundo_hook = \"" + hangCommand(pidFile) + "\"\n"
    );
    writeFile(root / "state" / "noctalia" / "state.toml", "[theme_templates]\napplied_builtin_ids = \"stuck\"\n");

    std::vector<pid_t> pids;
    const double elapsed = applyThenDestroy(root, "", {pidFile}, pids);

    TEST_CHECK(pids[0] > 0);
    TEST_CHECK(elapsed < 3.0);
    TEST_CHECK(processGone(pids[0]));
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-template-shutdown-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "noctalia");
  std::filesystem::create_directories(root / "state" / "noctalia");
  std::filesystem::create_directories(root / "data");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);
  writeFile(root / "input.tmpl", "static content, this test is about hooks\n");
  // A minimal asset bundle, so the built-in template config can be replaced.
  std::filesystem::create_directories(root / "assets" / "fonts");
  std::filesystem::create_directories(root / "assets" / "templates");
  std::filesystem::create_directories(root / "assets" / "translations");
  writeFile(root / "assets" / "emoji.json", "[]\n");
  writeFile(root / "assets" / "fonts" / "noctalia-tabler.ttf", "");
  writeFile(root / "assets" / "templates" / "builtin.toml", "");
  writeFile(root / "assets" / "translations" / "en.json", "{}\n");
  ::setenv("NOCTALIA_ASSETS_DIR", (root / "assets").c_str(), 1);

  test_stuck_inline_post_hook(root);
  test_stuck_sync_and_async_hooks_share_one_grace(root);
  test_stuck_undo_hook(root);

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  ::unsetenv("NOCTALIA_ASSETS_DIR");
  std::filesystem::remove_all(root);
  return 0;
}
