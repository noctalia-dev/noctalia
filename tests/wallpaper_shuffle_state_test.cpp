#include "shell/wallpaper/wallpaper_shuffle_state.h"
#include "tests/test_check.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

  std::filesystem::path gTempDir;

  void removeTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(gTempDir, ec);
  }

} // namespace

int main() {
  namespace fs = std::filesystem;

  gTempDir = fs::temp_directory_path()
      / ("noctalia-wallpaper-shuffle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::atexit(removeTempDir);
  const fs::path statePath = gTempDir / "wallpaper_shuffle.json";
  const std::vector<std::string> candidates{"a.jpg", "b.jpg", "c.jpg"};

  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("global", "default", candidates, "a.jpg", 0.0F) == "b.jpg");
  }
  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("global", "default", candidates, "b.jpg", 0.0F) == "c.jpg");
  }
  {
    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("global", "default", candidates, "c.jpg", 0.0F) == "a.jpg");
    TEST_CHECK(state.pick("output:DP-1", "default", candidates, "a.jpg", 1.0F) == "c.jpg");
  }
  {
    wallpaper::ShuffleState state;
    const std::vector<std::string> changedCandidates{"b.jpg", "c.jpg", "d.jpg"};
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("global", "default", changedCandidates, "a.jpg", 1.0F) == "d.jpg");
    TEST_CHECK(state.pick("global", "default", changedCandidates, "d.jpg", 0.0F) == "b.jpg");
  }
  {
    wallpaper::ShuffleState state;
    const std::vector<std::string> darkCandidates{"dark-a.jpg", "dark-b.jpg", "dark-c.jpg"};
    const std::vector<std::string> lightCandidates{"light-a.jpg", "light-b.jpg", "light-c.jpg"};
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("theme", "dark", darkCandidates, "dark-a.jpg", 0.0F) == "dark-b.jpg");
    TEST_CHECK(state.pick("theme", "light", lightCandidates, "dark-b.jpg", 0.0F) == "light-a.jpg");
  }
  {
    wallpaper::ShuffleState state;
    const std::vector<std::string> darkCandidates{"dark-a.jpg", "dark-b.jpg", "dark-c.jpg"};
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("theme", "dark", darkCandidates, "light-a.jpg", 0.0F) == "dark-c.jpg");
  }
  {
    std::ofstream malformed(statePath, std::ios::trunc);
    malformed << "not json\n";
    malformed.close();

    wallpaper::ShuffleState state;
    state.setStatePath(statePath);
    TEST_CHECK(state.pick("global", "default", candidates, "a.jpg", 0.0F) == "b.jpg");
    TEST_CHECK(state.pick("global", "default", {"only.jpg"}, "only.jpg", 0.0F) == "only.jpg");
  }

  return 0;
}
