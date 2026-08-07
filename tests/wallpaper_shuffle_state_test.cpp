#include "shell/wallpaper/wallpaper_shuffle_state.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::fprintf(stderr, "wallpaper_shuffle_state_test: %s\n", message);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  namespace fs = std::filesystem;

  const fs::path tempDir = fs::temp_directory_path()
      / ("noctalia-wallpaper-shuffle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path statePath = tempDir / "wallpaper_shuffle.json";
  const std::vector<std::string> candidates{"a.jpg", "b.jpg", "c.jpg"};

  bool ok = true;
  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    ok = expect(state.pick("global", candidates, "a.jpg", 0.0F) == "b.jpg", "first pick excludes current") && ok;
  }
  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    ok = expect(
             state.pick("global", candidates, "b.jpg", 0.0F) == "c.jpg",
             "persisted cycle does not replace a previously shown wallpaper"
         )
        && ok;
  }
  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    ok = expect(state.pick("global", candidates, "c.jpg", 0.0F) == "a.jpg", "new cycle starts after exhaustion") && ok;
    ok = expect(
             state.pick("output:DP-1", candidates, "a.jpg", 1.0F) == "c.jpg",
             "output scopes keep independent shuffle cycles"
         )
        && ok;
  }
  {
    wallpaper::ShuffleState state;
    const std::vector<std::string> changedCandidates{"b.jpg", "c.jpg", "d.jpg"};
    state.setStatePath(statePath);
    ok = expect(
             state.pick("global", changedCandidates, "a.jpg", 1.0F) == "d.jpg",
             "removed and newly added candidates reconcile with persisted state"
         )
        && ok;
    ok = expect(
             state.pick("global", changedCandidates, "d.jpg", 0.0F) == "b.jpg",
             "remaining changed candidates are used before replacement"
         )
        && ok;
  }
  {
    std::ofstream malformed(statePath, std::ios::trunc);
    malformed << "not json\n";
    malformed.close();

    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    ok = expect(state.pick("global", candidates, "a.jpg", 0.0F) == "b.jpg", "malformed state starts a fresh cycle")
        && ok;
    ok = expect(state.pick("global", {"only.jpg"}, "only.jpg", 0.0F) == "only.jpg", "single candidate is stable") && ok;
  }

  std::error_code ec;
  fs::remove_all(tempDir, ec);
  return ok ? 0 : 1;
}
