#include "ui/dialogs/file_artwork.h"
#include "ui/dialogs/file_view_metrics.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <limits>

int main() {
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
  // Presentation queries must leave both the links and the target untouched.
  assert(std::filesystem::is_symlink(std::filesystem::symlink_status(root / "folder-link")));
  assert(std::filesystem::exists(root / "target/readme.txt"));
  std::filesystem::remove_all(root);
}
