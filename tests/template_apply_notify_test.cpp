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
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

  using noctalia::theme::GeneratedPalette;
  using noctalia::theme::TemplateApplyService;

  GeneratedPalette paletteWith(std::uint32_t surface) {
    GeneratedPalette palette;
    palette.dark["mSurface"] = surface;
    palette.light["mSurface"] = surface;
    return palette;
  }

  // The worker applies once the request stream has been quiet for its coalescing window and
  // posts the notification through the deferred-call queue the main loop drains.
  void settle() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
      for (auto& call : DeferredCall::takePending()) {
        call();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& call : DeferredCall::takePending()) {
      call();
    }
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-template-notify-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config");
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

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  std::filesystem::remove_all(root);
  return 0;
}
