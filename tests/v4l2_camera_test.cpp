#include "system/v4l2_camera.h"
#include "test_check.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  void writeFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
    TEST_CHECK(file.good());
  }

  void addProcess(const fs::path& proc, const std::string& pid, const std::string& app, const fs::path& device) {
    writeFile(proc / pid / "comm", app + '\n');
    fs::create_directories(proc / pid / "fd");
    fs::create_symlink(device, proc / pid / "fd/3");
  }
} // namespace

int main() {
  std::string pattern = (fs::temp_directory_path() / "noctalia-v4l2-XXXXXX").string();
  const char* result = ::mkdtemp(pattern.data());
  TEST_CHECK(result != nullptr);
  const fs::path root(result);
  const auto video = root / "video4linux";
  const auto proc = root / "proc";
  auto scan = [&] { return v4l2CameraApps(video, proc); };

  TEST_CHECK(scan().empty());
  writeFile(video / "video0/name", "USB Camera\n");
  writeFile(video / "video1/name", "USB Camera Metadata\n");
  writeFile(video / "video2/name", "External Camera\n");
  writeFile(video / "video-invalid/name", "Invalid\n");
  fs::create_directories(video / "video3"); // Unreadable/missing device name.
  TEST_CHECK(scan().empty());

  addProcess(proc, "101", "firefox", "/dev/video0");
  TEST_CHECK(scan() == std::vector<std::string>{"firefox"});

  // Multiple descriptors, cameras and processes for the same app produce one entry.
  fs::create_symlink("/dev/video2", proc / "101/fd/4");
  addProcess(proc, "102", "firefox", "/dev/video2");
  addProcess(proc, "103", "chromium", "/dev/video0");
  const std::vector<std::string> both{"chromium", "firefox"};
  TEST_CHECK(scan() == both);

  addProcess(proc, "104", "metadata", "/dev/video1");
  addProcess(proc, "105", "unknown", "/dev/video3");
  addProcess(proc, "106", "invalid", "/dev/video-invalid");
  addProcess(proc, "107", "unrelated", "/tmp/video0");
  addProcess(proc, "108", "prefix", "/dev/video01");
  addProcess(proc, "109", "pipewire", "/dev/video0");
  addProcess(proc, "110", "wireplumber", "/dev/video2");
  addProcess(proc, "self", "alias", "/dev/video0");
  addProcess(proc, "111", "", "/dev/video0");
  addProcess(proc, "112", "exited", "/dev/video0");
  fs::remove(proc / "112/comm");
  writeFile(proc / "101/fd/5", "not a symlink");
  TEST_CHECK(scan() == both);

  // An inaccessible process must not prevent finding other camera users.
  addProcess(proc, "113", "private", "/dev/video0");
  fs::permissions(proc / "113/fd", fs::perms::none);
  TEST_CHECK(scan() == both);
  fs::permissions(proc / "113/fd", fs::perms::owner_all);
  fs::remove_all(proc / "113");

  // Close and process-exit transitions must clear stale activity.
  fs::remove_all(proc / "101/fd");
  fs::remove_all(proc / "102");
  TEST_CHECK(scan() == std::vector<std::string>{"chromium"});
  fs::remove(proc / "103/fd/3");
  TEST_CHECK(scan().empty());

  // Devices added and removed after startup are discovered on the next scan.
  addProcess(proc, "114", "hotplug", "/dev/video10");
  TEST_CHECK(scan().empty());
  writeFile(video / "video10/name", "Hotplug Camera\n");
  TEST_CHECK(scan() == std::vector<std::string>{"hotplug"});
  fs::remove_all(video / "video10");
  TEST_CHECK(scan().empty());
  fs::remove_all(root);
}
