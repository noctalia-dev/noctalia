// A palette-change notification is owed until an application delivers it. Later applies that
// coalesce with, supersede, or deduplicate against the requesting one must not swallow it, and
// an application that did not change the palette must stay silent.

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

  int notifications = 0;
  {
    ConfigService config;
    TemplateApplyService service(config);
    service.setPaletteChangedCallback([&]() { ++notifications; });

    // A plain application that changed the palette.
    service.apply(paletteWith(0x111111), "dark", /*force=*/false, /*paletteChanged=*/true);
    settle();
    TEST_CHECK(notifications == 1);

    // A same-palette apply deduplicates against the queued one. It must not cancel the palette
    // change the queued one is still owed.
    notifications = 0;
    service.apply(paletteWith(0x222222), "dark", /*force=*/false, /*paletteChanged=*/true);
    service.apply(paletteWith(0x222222), "dark", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(notifications == 1);

    // A different palette supersedes the queued request before it runs. The superseding
    // generation inherits the owed palette change instead of dropping it.
    notifications = 0;
    service.apply(paletteWith(0x333333), "dark", /*force=*/false, /*paletteChanged=*/true);
    service.apply(paletteWith(0x444444), "dark", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(notifications == 1);

    // An application that only switched mode re-renders templates but is not a palette change.
    notifications = 0;
    service.apply(paletteWith(0x444444), "light", /*force=*/false, /*paletteChanged=*/false);
    settle();
    TEST_CHECK(notifications == 0);

    // A palette change owed by an apply that has nothing left to render still lands.
    notifications = 0;
    service.apply(paletteWith(0x444444), "light", /*force=*/false, /*paletteChanged=*/true);
    settle();
    TEST_CHECK(notifications == 1);
  }

  ::unsetenv("NOCTALIA_CONFIG_HOME");
  ::unsetenv("NOCTALIA_STATE_HOME");
  ::unsetenv("NOCTALIA_DATA_HOME");
  std::filesystem::remove_all(root);
  return 0;
}
