#include "filemanager/file_sort.h"

#include <cassert>
#include <chrono>
#include <vector>

namespace {

  FileEntry entry(std::string name, std::string path, bool dir = false) {
    FileEntry result;
    result.name = std::move(name);
    result.absPath = std::move(path);
    result.isDir = dir;
    return result;
  }

  template <typename Compare> void assertStrictWeakOrdering(const std::vector<FileEntry>& values, Compare compare) {
    for (const auto& a : values) {
      assert(!compare(a, a));
      for (const auto& b : values) {
        assert(!(compare(a, b) && compare(b, a)));
        for (const auto& c : values)
          if (compare(a, b) && compare(b, c))
            assert(compare(a, c));
      }
    }
  }

} // namespace

int main() {
  using Field = FileDialogSortField;
  using Order = FileDialogSortOrder;
  auto old = entry("old.txt", "/old.txt");
  auto fresh = entry("fresh.txt", "/fresh.txt");
  auto unknown = entry("unknown.txt", "/unknown.txt");
  old.size = 2;
  fresh.size = 7;
  old.mtime = std::filesystem::file_time_type{} + std::chrono::seconds(10);
  fresh.mtime = std::filesystem::file_time_type{} + std::chrono::seconds(20);
  old.created = 100;
  old.hasCreated = true;
  fresh.created = 200;
  fresh.hasCreated = true;
  old.accessed = 300;
  old.hasAccessed = true;
  fresh.accessed = 400;
  fresh.hasAccessed = true;
  old.type = "Text document";
  fresh.type = "Image";

  for (Field field : {Field::Name, Field::Size, Field::Modified, Field::Created, Field::Accessed, Field::Type}) {
    const auto less = [=](const auto& a, const auto& b) {
      return filemanager::fileEntryLess(a, b, field, Order::Ascending, false, false);
    };
    assert(less(old, fresh) != less(fresh, old));
    assertStrictWeakOrdering(
        std::vector<FileEntry>{old, fresh, unknown, entry(std::string(1, static_cast<char>(0xff)), "/invalid")}, less
    );
  }
  for (Field field : {Field::Created, Field::Accessed, Field::Type}) {
    const auto less = [=](const auto& a, const auto& b) {
      return filemanager::fileEntryLess(a, b, field, Order::Descending, false, false);
    };
    assert(less(old, unknown)); // unavailable values stay last, even descending
    assert(!less(unknown, old));
    assertStrictWeakOrdering(
        std::vector<FileEntry>{old, fresh, unknown, entry(std::string(1, static_cast<char>(0xff)), "/invalid")}, less
    );
  }

  auto folder = entry("folder", "/folder", true);
  auto hiddenFolder = entry(".hidden-folder", "/.hidden-folder", true);
  auto hiddenFile = entry(".hidden-file", "/.hidden-file");
  auto visibleFile = entry("visible-file", "/visible-file");
  for (Order order : {Order::Ascending, Order::Descending}) {
    const auto less = [=](const auto& a, const auto& b) {
      return filemanager::fileEntryLess(a, b, Field::Name, order, true, true);
    };
    assert(less(folder, visibleFile));
    assert(less(visibleFile, hiddenFolder));
    assert(less(hiddenFolder, hiddenFile));
    assertStrictWeakOrdering(std::vector<FileEntry>{folder, hiddenFolder, hiddenFile, visibleFile}, less);
  }
}
