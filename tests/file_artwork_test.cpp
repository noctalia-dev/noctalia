#include "ui/dialogs/file_artwork.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

class FileIconTestAccess {
public:
  static const std::string& metadataIcon(const FileIcon& icon) { return icon.m_metadataIcon; }
};

namespace {
  bool waitForIcon(const FileIcon& icon, const std::string& name) {
    struct Wait {
      const FileIcon& icon;
      const std::string& name;
      GMainLoop* loop;
      bool matched = false;
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    } wait{icon, name, g_main_loop_new(nullptr, false)};
    g_timeout_add(
        1,
        [](gpointer data) -> gboolean {
          auto& state = *static_cast<Wait*>(data);
          state.matched = FileIconTestAccess::metadataIcon(state.icon) == state.name;
          if (state.matched || std::chrono::steady_clock::now() >= state.deadline) {
            g_main_loop_quit(state.loop);
            return G_SOURCE_REMOVE;
          }
          return G_SOURCE_CONTINUE;
        },
        &wait
    );
    g_main_loop_run(wait.loop);
    g_main_loop_unref(wait.loop);
    return wait.matched;
  }
} // namespace

int main() {
  g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL));
  const auto minimum = filemanager::fileViewMetrics(0.8F);
  const auto normal = filemanager::fileViewMetrics(1.0F);
  const auto maximum = filemanager::fileViewMetrics(1.8F);
  assert(minimum.listIcon == 16 && normal.listIcon == 22 && maximum.listIcon == 56);
  assert(minimum.gridIcon == 32 && normal.gridIcon == 48 && maximum.gridIcon == 112);
  for (const auto metrics : {minimum, normal, maximum}) {
    assert(metrics.listRowHeight >= metrics.listIcon + 10);
    assert(metrics.gridCellWidth >= metrics.gridIcon + 24);
    assert(metrics.gridCellHeight == metrics.gridIcon + 52);
  }
  assert(filemanager::fileViewMetrics(std::numeric_limits<float>::infinity()).listIcon == normal.listIcon);

  char* raw = g_dir_make_tmp("nythera-artwork-test-XXXXXX", nullptr);
  assert(raw != nullptr);
  const std::filesystem::path root(raw);
  g_free(raw);
  std::filesystem::create_directory(root / "target");
  {
    std::ofstream file(root / "target/readme.txt");
    file << "test";
  }
  std::filesystem::create_directory_symlink("target", root / "folder-link");
  std::filesystem::create_symlink("target/readme.txt", root / "file-link");
  std::filesystem::create_directory_symlink("missing", root / "broken");
  std::filesystem::create_symlink("loop", root / "loop");

  const auto folder = queryFileArtwork(root / "folder-link", true);
  assert(folder.symlink && !folder.broken);
  assert(std::ranges::find(folder.names, "folder") != folder.names.end());
  assert(queryFileTypeDescription(root / "folder-link") == "Folder (link to " + (root / "target").string() + ")");
  const auto fileType = queryFileTypeDescription(root / "file-link");
  assert(fileType.find("link to " + (root / "target/readme.txt").string()) != std::string::npos);
  assert(!fileType.starts_with("Folder"));
  const auto broken = queryFileArtwork(root / "broken", true);
  assert(broken.symlink && broken.broken);
  assert(std::ranges::find(broken.names, "folder") == broken.names.end());
  assert(queryFileTypeDescription(root / "broken").starts_with("Broken symbolic link"));
  assert(queryFileArtwork(root / "loop", false).broken);
  assert(!queryFileArtwork(root / "target", true).symlink);
  // Exercise the real async read AND close callback without a Wayland window.
  // The former close call could release its request before accessing its stream.
  {
    std::ofstream(root / "target/.directory") << "[Desktop Entry]\nType=Directory\nIcon=folder-games\n";
    FileIcon icon;
    icon.setFile(root / "target", true);
    assert(waitForIcon(icon, "folder-games"));
    // Rebinding cancels the previous request and rejects stale completions.
    std::filesystem::create_directory(root / "second");
    std::ofstream(root / "second/.directory") << "[Desktop Entry]\nType=Directory\nIcon=folder-music\n";
    icon.setFile(root / "target", false);
    icon.setFile(root / "target", true);
    icon.setFile(root / "second", true);
    assert(waitForIcon(icon, "folder-music"));
    {
      FileIcon cancelled;
      cancelled.setFile(root / "target", true);
    }
    // A fresh completion also drains cancelled and closing callbacks.
    icon.setFile(root / "target", true);
    assert(waitForIcon(icon, "folder-games"));
  }
  // Presentation queries must leave both the links and the target untouched.
  assert(std::filesystem::is_symlink(std::filesystem::symlink_status(root / "folder-link")));
  assert(std::filesystem::exists(root / "target/readme.txt"));
  std::filesystem::remove_all(root);
}
