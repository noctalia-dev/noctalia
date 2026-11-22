#include "test_isolation.h"
#include "ui/dialogs/file_artwork.h"
#include "ui/dialogs/file_icon.h"
#include "ui/dialogs/file_view_metrics.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

class FileIconTestAccess {
public:
  static const std::string& metadataIcon(const FileIcon& icon) { return icon.m_metadataIcon; }
  static bool artworkLoaded(const FileIcon& icon) { return icon.m_normalArtworkLoaded; }
  static bool isSymlink(const FileIcon& icon) { return icon.m_symlink; }
  static bool isBroken(const FileIcon& icon) { return icon.m_broken; }
  static const std::vector<std::string>& baseIconNames(const FileIcon& icon) { return icon.m_baseIconNames; }
  static void
  setArtworkQueryHook(std::function<bool(const std::filesystem::path&, bool, std::function<void(FileArtwork)>)> hook) {
    FileIcon::s_artworkQueryHook = std::move(hook);
  }
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
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-artwork-test"});
    const auto hookGuard = nythera::test::makeScopeExit([] { FileIconTestAccess::setArtworkQueryHook({}); });
    g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL));
    GFileInfo* missingType = g_file_info_new();
    TEST_CHECK(gFileInfoTypeOrUnknown(missingType) == G_FILE_TYPE_UNKNOWN);
    TEST_CHECK(!isBoundedDirectoryMetadataFile(missingType));
    g_file_info_set_attribute_uint64(missingType, G_FILE_ATTRIBUTE_STANDARD_SIZE, 12);
    TEST_CHECK(!isBoundedDirectoryMetadataFile(missingType));
    g_file_info_set_file_type(missingType, G_FILE_TYPE_REGULAR);
    TEST_CHECK(isBoundedDirectoryMetadataFile(missingType));
    g_file_info_set_attribute_uint64(missingType, G_FILE_ATTRIBUTE_STANDARD_SIZE, 65537);
    TEST_CHECK(!isBoundedDirectoryMetadataFile(missingType));
    g_file_info_set_attribute_uint64(missingType, G_FILE_ATTRIBUTE_STANDARD_SIZE, 12);
    g_file_info_set_file_type(missingType, G_FILE_TYPE_DIRECTORY);
    TEST_CHECK(gFileInfoTypeOrUnknown(missingType) == G_FILE_TYPE_DIRECTORY);
    TEST_CHECK(!isBoundedDirectoryMetadataFile(missingType));
    g_file_info_set_file_type(missingType, G_FILE_TYPE_SYMBOLIC_LINK);
    TEST_CHECK(gFileInfoTypeOrUnknown(missingType) == G_FILE_TYPE_SYMBOLIC_LINK);
    g_object_unref(missingType);
    const auto minimum = filemanager::fileViewMetrics(0.8F);
    const auto normal = filemanager::fileViewMetrics(1.0F);
    const auto maximum = filemanager::fileViewMetrics(1.8F);
    TEST_CHECK(minimum.listIcon == 16 && normal.listIcon == 22 && maximum.listIcon == 56);
    TEST_CHECK(minimum.gridIcon == 32 && normal.gridIcon == 48 && maximum.gridIcon == 112);
    for (const auto metrics : {minimum, normal, maximum}) {
      TEST_CHECK(metrics.listRowHeight >= metrics.listIcon + 10);
      TEST_CHECK(metrics.gridCellWidth >= metrics.gridIcon + 24);
      TEST_CHECK(metrics.gridCellHeight == metrics.gridIcon + 52);
    }
    TEST_CHECK(filemanager::fileViewMetrics(std::numeric_limits<float>::infinity()).listIcon == normal.listIcon);

    const auto& root = env.work();
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
    TEST_CHECK(folder.symlink && !folder.broken);
    TEST_CHECK(std::ranges::find(folder.names, "folder") != folder.names.end());
    TEST_CHECK(queryFileTypeDescription(root / "folder-link") == "Folder (link to " + (root / "target").string() + ")");
    const auto fileType = queryFileTypeDescription(root / "file-link");
    TEST_CHECK(fileType.find("link to " + (root / "target/readme.txt").string()) != std::string::npos);
    TEST_CHECK(!fileType.starts_with("Folder"));
    const auto broken = queryFileArtwork(root / "broken", true);
    TEST_CHECK(broken.symlink && broken.broken);
    TEST_CHECK(std::ranges::find(broken.names, "folder") == broken.names.end());
    TEST_CHECK(queryFileTypeDescription(root / "broken").starts_with("Broken symbolic link"));
    TEST_CHECK(queryFileArtwork(root / "loop", false).broken);
    TEST_CHECK(!queryFileArtwork(root / "target", true).symlink);
    // Exercise the real async read AND close callback without a Wayland window.
    // The former close call could release its request before accessing its stream.
    {
      std::ofstream(root / "target/.directory") << "[Desktop Entry]\nType=Directory\nIcon=folder-games\n";
      FileIcon icon;
      icon.setFile(root / "target", true);
      TEST_CHECK(waitForIcon(icon, "folder-games"));
      // Rebinding cancels the previous request and rejects stale completions.
      std::filesystem::create_directory(root / "second");
      std::ofstream(root / "second/.directory") << "[Desktop Entry]\nType=Directory\nIcon=folder-music\n";
      icon.setFile(root / "target", false);
      icon.setFile(root / "target", true);
      icon.setFile(root / "second", true);
      TEST_CHECK(waitForIcon(icon, "folder-music"));
      {
        FileIcon cancelled;
        cancelled.setFile(root / "target", true);
      }
      // A fresh completion also drains cancelled and closing callbacks.
      icon.setFile(root / "target", true);
      TEST_CHECK(waitForIcon(icon, "folder-games"));
    }

    // F-04: Async FileIcon artwork resolution (real GIO async + delayed/stale/destroyed hooks).
    {
      auto waitForPredicate = [](const std::function<bool()>& pred) {
        struct Wait {
          const std::function<bool()>& pred;
          GMainLoop* loop;
          bool matched = false;
          std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        } wait{pred, g_main_loop_new(nullptr, false)};
        g_timeout_add(
            1,
            [](gpointer data) -> gboolean {
              auto& state = *static_cast<Wait*>(data);
              state.matched = state.pred();
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
      };

      // Real GIO async artwork resolution for valid symlink and broken symlink.
      FileIcon asyncIcon;
      asyncIcon.setFile(root / "folder-link", true);
      TEST_CHECK(waitForPredicate([&] { return FileIconTestAccess::artworkLoaded(asyncIcon); }));
      TEST_CHECK(FileIconTestAccess::isSymlink(asyncIcon) && !FileIconTestAccess::isBroken(asyncIcon));

      asyncIcon.setFile(root / "broken", true);
      TEST_CHECK(waitForPredicate([&] { return FileIconTestAccess::artworkLoaded(asyncIcon); }));
      TEST_CHECK(FileIconTestAccess::isSymlink(asyncIcon) && FileIconTestAccess::isBroken(asyncIcon));

      // Deterministic delayed artwork response: non-blocking lexical fallback, stale rejection, and safe destruction.
      std::vector<std::pair<std::filesystem::path, std::function<void(FileArtwork)>>> pending;
      FileIconTestAccess::setArtworkQueryHook([&](const std::filesystem::path& path, bool,
                                                  std::function<void(FileArtwork)> complete) {
        pending.emplace_back(path, std::move(complete));
        return true;
      });

      FileIcon delayed;
      delayed.setFile(root / "first.txt", false);
      TEST_CHECK(!FileIconTestAccess::artworkLoaded(delayed));
      TEST_CHECK(!FileIconTestAccess::baseIconNames(delayed).empty());
      TEST_CHECK(pending.size() == 1);

      // Navigate/rebind before first completes.
      delayed.setFile(root / "second.png", false);
      TEST_CHECK(pending.size() == 2);

      // Stale completion for first.txt must be ignored.
      pending[0].second(FileArtwork{.names = {"stale-icon"}, .symlink = true, .broken = true});
      TEST_CHECK(!FileIconTestAccess::artworkLoaded(delayed));
      TEST_CHECK(!FileIconTestAccess::isSymlink(delayed));
      TEST_CHECK(
          std::ranges::find(FileIconTestAccess::baseIconNames(delayed), "stale-icon")
          == FileIconTestAccess::baseIconNames(delayed).end()
      );

      // Current completion for second.png must apply.
      pending[1].second(FileArtwork{.names = {"image-x-generic"}, .symlink = false, .broken = false});
      TEST_CHECK(FileIconTestAccess::artworkLoaded(delayed));
      TEST_CHECK(
          std::ranges::find(FileIconTestAccess::baseIconNames(delayed), "image-x-generic")
          != FileIconTestAccess::baseIconNames(delayed).end()
      );

      // Destruction while a delayed request is unfinished must be safe.
      pending.clear();
      {
        FileIcon transient;
        transient.setFile(root / "transient.txt", false);
        TEST_CHECK(pending.size() == 1);
      }
      pending[0].second(FileArtwork{.names = {"after-destroy"}, .symlink = false, .broken = false});
      FileIconTestAccess::setArtworkQueryHook({});
    }
    // Presentation queries must leave both the links and the target untouched.
    TEST_CHECK(std::filesystem::is_symlink(std::filesystem::symlink_status(root / "folder-link")));
    TEST_CHECK(std::filesystem::exists(root / "target/readme.txt"));
  });
}
