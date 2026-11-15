#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_interaction.h"
#include "filemanager/place_metadata.h"
#include "ui/dialogs/file_artwork.h"

#include <cassert>
#include <fstream>
#include <gio/gio.h>

int main() {
  char* raw = g_dir_make_tmp("nythera-drop-test-XXXXXX", nullptr);
  assert(raw);
  const std::filesystem::path root(raw);
  g_free(raw);
  std::filesystem::create_directories(root / "source/child");
  std::filesystem::create_directories(root / "destination");
  std::filesystem::create_directory_symlink(root / "source/child", root / "alias");
  std::ofstream(root / "file") << "fixture";
  const std::vector<std::string> sources{(root / "source").string(), (root / "file").string()};
  assert(filemanager::checkedDropTarget(sources, root / "destination"));
  assert(!filemanager::checkedDropTarget(sources, root));
  assert(!filemanager::checkedDropTarget(sources, root / "source"));
  assert(!filemanager::checkedDropTarget(sources, root / "source/child"));
  assert(!filemanager::checkedDropTarget(sources, root / "alias"));
  assert(!filemanager::checkedDropTarget(sources, root / "missing"));
  assert(!filemanager::checkedDropTarget(sources, root / "file"));
  assert(!filemanager::checkedDropTarget({}, root / "destination"));
  assert(!filemanager::copyDropByDefault(sources, root / "destination"));
  std::filesystem::create_symlink(root / "file", root / "file-link");
  std::filesystem::create_symlink(root / "missing", root / "broken-link");
  std::filesystem::create_directory_symlink(root / "destination", root / "outside-link");
  for (const auto* name : {"alias", "file-link", "outside-link"}) {
    const auto artwork = queryFileArtwork(root / name, false);
    assert(artwork.symlink && !artwork.broken && !artwork.names.empty());
    assert(artwork.names.back() == (std::string(name) == "file-link" ? "text-x-generic" : "folder"));
  }
  assert(filemanager::checkedDropTarget({(root / "broken-link").string()}, root / "destination"));
  assert(filemanager::checkedDropTarget({(root / "file-link").string()}, root / "destination"));
  assert(!filemanager::checkedDropTarget({(root / "broken-link").string()}, root));
  const auto broken = queryFileArtwork(root / "broken-link", true);
  assert(broken.symlink && broken.broken && broken.names.back() != "folder");
  assert(!queryFileArtwork(root / "file", false).symlink);
  const auto normalFolder = queryFileArtwork(root / "source/child", true);
  assert(queryFileArtwork(root / "alias", false).names == normalFolder.names);

  for (const auto* path :
       {"/", "/home/test", "/home/test/nested/", "trash:", "trash:/", "trash:///", "trash:///folder/"}) {
    const auto crumbs = filemanager::fileBreadcrumbs(path);
    assert(!crumbs.empty());
    for (const auto& crumb : crumbs)
      assert(!crumb.label.empty() && !crumb.path.empty());
    const bool trash = std::string(path).starts_with("trash:");
    assert(crumbs.front().label == (trash ? "Wastebin" : "Filesystem"));
    if (trash && std::string(path).find("folder") == std::string::npos)
      assert(crumbs.size() == 1);
  }
  const auto nested = filemanager::fileBreadcrumbs("trash:///folder/child/");
  assert(nested.size() == 3 && nested.back().path == "trash:/folder/child");
  assert(filemanager::fileBreadcrumbs(nested[1].path).size() == 2);

  std::filesystem::create_directory(root / "labels");
  std::filesystem::create_symlink(root / "file", root / "labels/Test\\x20Volume");
  assert(filemanager::filesystemLabel(root / "file", root / "labels") == "Test Volume");
  assert(filemanager::filesystemLabel(root / "missing", root / "labels").empty());
  std::filesystem::remove_all(root);
}
