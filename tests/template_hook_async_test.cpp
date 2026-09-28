#include "theme/hook_runner.h"
#include "theme/template_engine.h"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <thread>

namespace {

  using noctalia::theme::HookRunner;
  using noctalia::theme::TemplateEngine;

  const std::filesystem::path kRoot = std::filesystem::temp_directory_path() / "noctalia_hook_async_test";

  void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
  }

  std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  // Runs a template config against a real HookRunner and reports how long processing took.
  double
  process(const std::string& configText, HookRunner& runner, std::shared_ptr<std::atomic<bool>> hookCancel = nullptr) {
    const auto configPath = kRoot / "templates.toml";
    writeFile(configPath, configText);

    TemplateEngine::Options options;
    options.defaultMode = "dark";
    options.verbose = false;
    options.hookRunner = &runner;
    options.generation = 1;
    options.hookCancel = std::move(hookCancel);

    TemplateEngine engine(TemplateEngine::ThemeData{{"primary", {{"dark", "#ff0000"}}}}, options);
    const auto start = std::chrono::steady_clock::now();
    const bool ok = engine.processConfigFile(configPath);
    assert(ok);
    (void)ok;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }

  std::string entry(const std::string& name, const std::string& hook, bool async) {
    return "[templates."
        + name
        + "]\n"
        + "input_path = \"input.tmpl\"\n"
        + "output_path = \""
        + (kRoot / (name + ".out")).string()
        + "\"\n"
        + "post_hook = \""
        + hook
        + "\"\n"
        + (async ? "" : "hook_async = false\n")
        + "\n";
  }

  // The default: a slow post_hook does not hold up the templates behind it.
  void test_async_post_hook_does_not_block_processing() {
    const auto marker = kRoot / "async_marker";
    std::filesystem::remove(marker);

    HookRunner runner(4);
    const double elapsed = process(entry("slow", "sleep 1; touch " + marker.string(), /*async=*/true), runner);

    assert(elapsed < 0.5);
    assert(!std::filesystem::exists(marker)); // still running
    (void)elapsed;

    runner.waitIdle();
    assert(std::filesystem::exists(marker));
  }

  // hook_async = false serializes against hooks started earlier in the same run, so two
  // entries driving the same script never overlap.
  void test_inline_post_hook_waits_for_started_hooks() {
    const auto order = kRoot / "order";
    std::filesystem::remove(order);

    HookRunner runner(4);
    const std::string config = entry("background", "sleep 0.4; printf background, >> " + order.string(), /*async=*/true)
        + entry("inline", "printf inline, >> " + order.string(), /*async=*/false);

    const double elapsed = process(config, runner);
    runner.waitIdle();

    // The inline hook ran after the background one despite being enqueued later, and
    // processing had to wait for it.
    assert(readFile(order) == "background,inline,");
    assert(elapsed >= 0.4);
    (void)elapsed;
  }

  // An inline hook that never exits is terminated once the shutdown flag is raised,
  // instead of holding processing (and so the worker's join) forever.
  void test_inline_post_hook_is_terminated_by_hook_cancel() {
    const auto pidFile = kRoot / "hung_pid";
    std::filesystem::remove(pidFile);

    HookRunner runner(4);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    pid_t pid = -1;
    std::thread raiser([&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        if (const std::string contents = readFile(pidFile); !contents.empty()) {
          pid = static_cast<pid_t>(std::stol(contents));
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      cancel->store(true);
    });

    // Ignores SIGTERM, so only the SIGKILL escalation can end it.
    const std::string hook = "trap '' TERM; echo $$ > " + pidFile.string() + "; exec sleep 600";
    const double elapsed = process(entry("hung", hook, /*async=*/false), runner, cancel);
    raiser.join();

    assert(pid > 0);
    assert(elapsed < 2.0);
    assert(::kill(pid, 0) != 0 && errno == ESRCH);
    (void)elapsed;
  }

  // output_path_dynamic runs a user command on the worker too, so the same flag bounds it.
  void test_dynamic_path_command_is_terminated_by_hook_cancel() {
    const auto pidFile = kRoot / "dynamic_pid";
    std::filesystem::remove(pidFile);

    HookRunner runner(4);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    pid_t pid = -1;
    std::thread raiser([&]() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        if (const std::string contents = readFile(pidFile); !contents.empty()) {
          pid = static_cast<pid_t>(std::stol(contents));
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      cancel->store(true);
    });

    const std::string config = "[templates.dynamic]\n"
                               "input_path = \"input.tmpl\"\n"
                               "output_path_dynamic = \"trap '' TERM; echo $$ > "
        + pidFile.string()
        + "; exec sleep 600\"\n";
    const double elapsed = process(config, runner, cancel);
    raiser.join();

    assert(pid > 0);
    assert(elapsed < 2.0);
    assert(::kill(pid, 0) != 0 && errno == ESRCH);
    (void)elapsed;
  }

} // namespace

int main() {
  std::filesystem::remove_all(kRoot);
  std::filesystem::create_directories(kRoot);
  writeFile(kRoot / "input.tmpl", "static content, this test is about hooks\n");

  test_async_post_hook_does_not_block_processing();
  test_inline_post_hook_waits_for_started_hooks();
  test_inline_post_hook_is_terminated_by_hook_cancel();
  test_dynamic_path_command_is_terminated_by_hook_cancel();

  std::filesystem::remove_all(kRoot);
  return 0;
}
