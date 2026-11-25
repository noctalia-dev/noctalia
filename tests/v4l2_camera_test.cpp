#include "system/v4l2_camera.h"
#include "tests/test_check.h"

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
  TEST_CHECK(v4l2CameraSameApp("Firefox", "firefox"));
  TEST_CHECK(v4l2CameraSameApp("/usr/lib64/firefox/firefox", "firefox"));
  TEST_CHECK(v4l2CameraSameApp("WebKitWebProcess", "WebKitWebProces"));
  TEST_CHECK(!v4l2CameraSameApp("firefox", "firefox-bin"));
  TEST_CHECK(!v4l2CameraSameApp("", "firefox"));
  TEST_CHECK(!v4l2CameraSameApp("Firefox", "chromium"));

  std::string pattern = (fs::temp_directory_path() / "noctalia-v4l2-XXXXXX").string();
  const char* result = ::mkdtemp(pattern.data());
  TEST_CHECK(result != nullptr);
  const fs::path root(result);
  const auto video = root / "video4linux";
  const auto proc = root / "proc";
  const auto udev = root / "udev";
  auto scan = [&] { return v4l2CameraApps(video, proc, udev); };

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

  // A process whose fd directory cannot be listed must not prevent finding other camera users.
  // A non-directory fd fails like a permission error, but root (as in CI) cannot bypass it.
  writeFile(proc / "113/comm", "private\n");
  writeFile(proc / "113/fd", "not a directory");
  TEST_CHECK(scan() == both);
  fs::remove_all(proc / "113");

  // Close and process-exit transitions must clear stale activity.
  fs::remove_all(proc / "101/fd");
  fs::remove_all(proc / "102");
  TEST_CHECK(scan() == std::vector<std::string>{"chromium"});
  fs::remove(proc / "103/fd/3");
  TEST_CHECK(scan().empty());

  // udev's capability record decides when present: mem2mem codecs (no capture, or capture plus
  // video_output on older drivers) are not cameras, a capture record keeps the node, and nodes
  // without a record fall back to the name check.
  writeFile(video / "video0/dev", "81:0\n");
  writeFile(udev / "c81:0", "E:ID_V4L_PRODUCT=USB Camera\nE:ID_V4L_CAPABILITIES=:capture:\n");
  writeFile(video / "video4/name", "avd\n");
  writeFile(video / "video4/dev", "81:4\n");
  writeFile(udev / "c81:4", "E:ID_V4L_CAPABILITIES=:\n");
  writeFile(video / "video5/name", "mtk-vcodec-enc\n");
  writeFile(video / "video5/dev", "81:5\n");
  writeFile(udev / "c81:5", "E:ID_V4L_CAPABILITIES=:capture:video_output:\n");
  writeFile(video / "video6/name", "Unrecorded Camera\n");
  writeFile(video / "video6/dev", "81:6\n");
  addProcess(proc, "115", "decoder", "/dev/video4");
  addProcess(proc, "116", "encoder", "/dev/video5");
  TEST_CHECK(scan().empty());
  addProcess(proc, "117", "camera", "/dev/video0");
  addProcess(proc, "118", "unrecorded", "/dev/video6");
  TEST_CHECK(scan() == (std::vector<std::string>{"camera", "unrecorded"}));
  fs::remove_all(proc / "117");
  fs::remove_all(proc / "118");
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
