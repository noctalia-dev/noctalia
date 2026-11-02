// A requested after-apply notification is owed until an application delivers it. Later
// applies that coalesce with, supersede, or deduplicate against the requesting one must not
// swallow it, and an application that did not ask for one must stay silent.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "tests/test_check.h"
#include "theme/palette.h"
#include "theme/template_apply_service.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

  using noctalia::theme::GeneratedPalette;
  using noctalia::theme::TemplateApplyService;

  GeneratedPalette paletteWith(std::uint32_t surface) {
    GeneratedPalette palette;
    palette.dark["mSurface"] = surface;
    palette.light["mSurface"] = surface;
    return palette;
  }

  void drainDeferredCalls() {
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
  }

  template <typename Condition> void waitFor(Condition condition, bool drain = true) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
      TEST_CHECK(std::chrono::steady_clock::now() < deadline);
      if (drain) {
        drainDeferredCalls();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path);
    out << text;
    TEST_CHECK(out.good());
  }

  // The worker applies once the request stream has been quiet for its coalescing window and
  // posts the notification through the deferred-call queue the main loop drains.
  void settle() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
      drainDeferredCalls();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    drainDeferredCalls();
  }

  void testHookWarnings(const std::filesystem::path& root, bool async) {
    const auto fixture = root / (async ? "async" : "inline");
    std::filesystem::create_directories(fixture);
    const auto success = fixture / "success";
    const auto block = fixture / "block";
    const auto started = fixture / "started";
    const auto release = fixture / "release";
    const auto failed = fixture / "failed";
    const auto hook = fixture / "hook.sh";
    const std::string diagnostic = "Warning: Missing compositor config; add include manually";
    writeFile(
        hook,
        "if [ -e '"
            + success.string()
            + "' ]; then exit 0; fi\n"
              "if [ -e '"
            + block.string()
            + "' ]; then\n"
              "  : > '"
            + started.string()
            + "'\n"
              "  while [ ! -e '"
            + release.string()
            + "' ]; do sleep 0.01; done\n"
              "fi\n"
              "printf '%s\\n' '"
            + diagnostic
            + "' >&2\n"
              ": > '"
            + failed.string()
            + "'\nexit 1\n"
    );
    writeFile(root / "input.tmpl", "generated theme\n");
    writeFile(
        root / "config" / "noctalia" / "config.toml",
        "[theme.templates.user.warning]\n"
        "enabled = true\n"
        "input_path = \""
            + (root / "input.tmpl").string()
            + "\"\n"
              "output_path = \""
            + (fixture / "theme.out").string()
            + "\"\n"
              "post_hook = \"sh '"
            + hook.string()
            + "'\"\n"
            + (async ? "" : "hook_async = false\n")
            + "\n[theme.templates.user.other]\n"
              "enabled = true\n"
              "input_path = \""
            + (root / "input.tmpl").string()
            + "\"\n"
              "output_path = \""
            + (fixture / "other.out").string()
            + "\"\n"
              "post_hook = \"sh '"
            + hook.string()
            + "'\"\n"
            + (async ? "" : "hook_async = false\n")
    );

    ConfigService config;
    TemplateApplyService service(config);
    const auto mainThread = std::this_thread::get_id();
    std::vector<std::string> warnings;
    int applications = 0;
    service.setWarningCallback([&](std::string_view warning) {
      TEST_CHECK(std::this_thread::get_id() == mainThread);
      warnings.emplace_back(warning);
    });
    service.setAfterApplyCallback([&](std::string_view, bool) { ++applications; });
    auto apply = [&]() { service.apply(paletteWith(0x555555), "dark", /*force=*/true, /*paletteChanged=*/false); };

    apply();
    waitFor([&]() { return std::filesystem::exists(failed); }, /*drain=*/false);
    TEST_CHECK(warnings.empty());
    waitFor([&]() { return applications == 1; });
    TEST_CHECK(warnings.size() == 1);
    TEST_CHECK(warnings.back().contains(diagnostic));
    TEST_CHECK(warnings.back().contains("warning"));
    TEST_CHECK(warnings.back().contains("other"));

    // A dismissed Settings error must be shown again when a forced retry still fails.
    apply();
    waitFor([&]() { return applications == 2; });
    TEST_CHECK(warnings.size() == 2);
    TEST_CHECK(warnings[0] == warnings[1]);

    writeFile(success, "");
    apply();
    waitFor([&]() { return applications == 3; });
    TEST_CHECK(warnings.size() == 2);
    std::filesystem::remove(success);
    apply();
    waitFor([&]() { return applications == 4; });
    TEST_CHECK(warnings.size() == 3);

    // A running failure belongs to its generation even if its replacement succeeds.
    writeFile(block, "");
    apply();
    waitFor([&]() { return std::filesystem::exists(started); }, /*drain=*/false);
    writeFile(success, "");
    apply();
    writeFile(release, "");
    waitFor([&]() { return applications == 5; });
    TEST_CHECK(warnings.size() == 3);

    // Even a completed warning waiting in the main-loop queue becomes stale on retry.
    std::filesystem::remove(block);
    std::filesystem::remove(success);
    apply();
    std::vector<std::function<void()>> pending;
    waitFor(
        [&]() {
          pending = DeferredCall::takePending();
          return !pending.empty();
        },
        /*drain=*/false
    );
    writeFile(success, "");
    apply();
    for (auto& call : pending) {
      call();
    }
    TEST_CHECK(warnings.size() == 3);
    waitFor([&]() { return applications == 7; });
    TEST_CHECK(warnings.size() == 3);

    // Ordinary hook failures stay in the log instead of becoming setup banners.
    writeFile(hook, "printf '%s\\n' 'ordinary hook failure' >&2\nexit 1\n");
    apply();
    waitFor([&]() { return applications == 8; });
    TEST_CHECK(warnings.size() == 3);

    writeFile(hook, "exit 1\n");
    apply();
    waitFor([&]() { return applications == 9; });
    TEST_CHECK(warnings.size() == 3);
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-template-notify-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "noctalia");
  std::filesystem::create_directories(root / "state");
  std::filesystem::create_directories(root / "data");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

  int applications = 0;
  int paletteChanges = 0;
  std::string lastMode;
  {
    ConfigService config;
    TemplateApplyService service(config);
    service.setAfterApplyCallback([&](std::string_view appliedMode, bool paletteChanged) {
      ++applications;
      paletteChanges += paletteChanged ? 1 : 0;
      lastMode = appliedMode;
    });

    // A plain application that changed the palette.
    service.apply(paletteWith(0x111111), "dark", /*force=*/false, /*paletteChanged=*/true);
    settle();
    TEST_CHECK(applications == 1);
    TEST_CHECK(paletteChanges == 1);
    TEST_CHECK(lastMode == "dark");

    // A same-palette apply deduplicates against the queued one. It must not cancel the palette
    // change the queued one is still owed.
    applications = 0;
    paletteChanges = 0;
    service.apply(paletteWith(0x222222), "dark", /*force=*/false, /*paletteChanged=*/true);
    service.apply(paletteWith(0x222222), "dark", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(applications == 1);
    TEST_CHECK(paletteChanges == 1);

    // A different palette supersedes the queued request before it runs. The superseding
    // generation inherits the owed palette change instead of dropping it.
    applications = 0;
    paletteChanges = 0;
    service.apply(paletteWith(0x333333), "dark", /*force=*/false, /*paletteChanged=*/true);
    service.apply(paletteWith(0x444444), "dark", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(applications == 1);
    TEST_CHECK(paletteChanges == 1);

    // An application that only switched mode still reports, so a consumer ordered behind the
    // templates runs, but it is not a palette change.
    applications = 0;
    paletteChanges = 0;
    service.apply(paletteWith(0x444444), "light", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(applications == 1);
    TEST_CHECK(paletteChanges == 0);
    TEST_CHECK(lastMode == "light");

    // A palette change owed by an apply that has nothing left to render still lands.
    applications = 0;
    paletteChanges = 0;
    service.apply(paletteWith(0x444444), "light", /*force=*/false, /*paletteChanged=*/true);
    settle();
    TEST_CHECK(applications == 1);
    TEST_CHECK(paletteChanges == 1);
  }

  testHookWarnings(root, /*async=*/true);
  testHookWarnings(root, /*async=*/false);

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  std::filesystem::remove_all(root);
  return 0;
}
