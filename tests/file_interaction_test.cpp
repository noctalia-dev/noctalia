#include "filemanager/external_tools.h"
#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_creation.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/place_metadata.h"
#include "test_isolation.h"
#include "ui/dialogs/file_artwork.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-file-interaction"});
    constexpr auto testHome = "/home/example";
    TEST_CHECK(filemanager::expandHomePath("~", testHome) == testHome);
    TEST_CHECK(filemanager::expandHomePath("~/", testHome) == testHome);
    TEST_CHECK(filemanager::expandHomePath("~/.config", testHome) == "/home/example/.config");
    TEST_CHECK(filemanager::expandHomePath("~/Documents", testHome) == "/home/example/Documents");
    TEST_CHECK(filemanager::expandHomePath("~/Documents", "/") == "/Documents");
    for (const auto* unchanged :
         {"/mnt/Media", "../folder/./item", "folder/~", "~other/.config", "$HOME", "file:///tmp/~/file", "trash:/", ""})
      TEST_CHECK(filemanager::expandHomePath(unchanged, testHome) == unchanged);

    nythera::test::ScopedTempDir creationTempDir("nythera-file-creation-test");
    const auto& creationRoot = creationTempDir.path();
    std::error_code creationError;
    const auto emptyFile = filemanager::creationDestination(creationRoot, "notes", false, creationError);
    TEST_CHECK(emptyFile && *emptyFile == creationRoot / "notes" && !creationError);
    std::ofstream(*emptyFile).put('x');
    const auto collision = filemanager::creationDestination(creationRoot, "notes", false, creationError);
    TEST_CHECK(collision && *collision == creationRoot / "notes 2" && !creationError);
    const auto textFile = filemanager::creationDestination(creationRoot, "readme.TXT", true, creationError);
    TEST_CHECK(textFile && *textFile == creationRoot / "readme.TXT" && !creationError);
    const auto textFileWithExtension = filemanager::creationDestination(creationRoot, "readme", true, creationError);
    TEST_CHECK(textFileWithExtension && *textFileWithExtension == creationRoot / "readme.txt" && !creationError);
    TEST_CHECK(!filemanager::creationDestination(creationRoot, "../escape", false, creationError) && creationError);

    const auto verifyMenuGroups = [](std::vector<ContextMenuControlEntry> entries, filemanager::FileMenuContext context,
                                     const std::vector<int>& expectedIds) {
      filemanager::groupFileMenuEntries(entries, context);
      std::vector<int> actualIds;
      bool previousSeparator = true;
      for (const auto& entry : entries) {
        TEST_CHECK(!entry.header);
        if (entry.separator) {
          TEST_CHECK(!previousSeparator);
          previousSeparator = true;
        } else {
          actualIds.push_back(entry.id);
          previousSeparator = false;
        }
      }
      TEST_CHECK(!previousSeparator);
      TEST_CHECK(actualIds == expectedIds);
    };
    verifyMenuGroups(
        {{.id = 22},
         {.separator = true},
         {.separator = true},
         {.id = 100},
         {.id = 6},
         {.id = 20},
         {.id = 3},
         {.id = 110},
         {.id = 10},
         {.id = 4},
         {.id = 1},
         {.id = 5},
         {.id = 2},
         {.id = 59},
         {.id = 60},
         {.id = 31}},
        filemanager::FileMenuContext::File, {10, 60, 2, 1, 3, 59, 110, 4, 5, 6, 20, 31, 100, 22}
    );
    verifyMenuGroups(
        {{.id = 21}, {.id = 100}, {.id = 50}, {.id = 110}, {.id = 14}, {.id = 3}}, filemanager::FileMenuContext::Empty,
        {3, 110, 14, 50, 100, 21}
    );
    verifyMenuGroups(
        {{.id = 39},
         {.id = 38},
         {.id = 34},
         {.id = 30},
         {.id = 37},
         {.id = 33},
         {.id = 32},
         {.id = 40},
         {.id = 31},
         {.id = 35},
         {.id = 36}},
        filemanager::FileMenuContext::Places, {30, 40, 33, 34, 35, 31, 32, 36, 37, 38, 39}
    );
    std::vector<ContextMenuControlEntry> optionalEntries{{.id = 21}};
    filemanager::groupFileMenuEntries(optionalEntries, filemanager::FileMenuContext::Empty);
    TEST_CHECK(
        optionalEntries.size() == 1 && optionalEntries.front().id == 21
    ); // Empty optional groups leave no rules.
    std::vector<ContextMenuControlEntry> disabledEntries{{.id = 4, .enabled = false}, {.id = 10}};
    filemanager::groupFileMenuEntries(disabledEntries, filemanager::FileMenuContext::File);
    TEST_CHECK(disabledEntries.size() == 3 && disabledEntries.front().id == 10 && !disabledEntries.back().enabled);

    TEST_CHECK(filemanager::locationEditCancelIsCurrent(true, 3, 3));
    TEST_CHECK(!filemanager::locationEditCancelIsCurrent(false, 3, 3));
    TEST_CHECK(!filemanager::locationEditCancelIsCurrent(true, 4, 3));
    TEST_CHECK(filemanager::pointerOutsideRect(9, 20, 10, 10, 40, 20));
    TEST_CHECK(!filemanager::pointerOutsideRect(10, 10, 10, 10, 40, 20));
    TEST_CHECK(!filemanager::pointerOutsideRect(49, 29, 10, 10, 40, 20));

    const auto verifyContextSortEntry = [](std::vector<ContextMenuControlEntry> entries,
                                           filemanager::FileMenuContext context) {
      filemanager::appendFileMenuEntries(
          entries, context, true, false, FileDialogSortField::Name, FileDialogSortOrder::Descending, false, true
      );
      TEST_CHECK(entries.back().id == 100 && entries.back().hasSubmenu);
      TEST_CHECK(std::ranges::count(entries, 100, &ContextMenuControlEntry::id) == 1);
      const bool hasViewActions = context == filemanager::FileMenuContext::Empty;
      TEST_CHECK((std::ranges::find(entries, 14, &ContextMenuControlEntry::id) != entries.end()) == hasViewActions);
      TEST_CHECK((std::ranges::find(entries, 50, &ContextMenuControlEntry::id) != entries.end()) == hasViewActions);
      const auto& sortEntries = entries.back().submenuEntries;
      TEST_CHECK(sortEntries.size() == 10);
      const auto control = [&sortEntries](int id) -> const ContextMenuControlEntry& {
        const auto found = std::ranges::find(sortEntries, id, &ContextMenuControlEntry::id);
        TEST_CHECK(found != sortEntries.end());
        return *found;
      };
      TEST_CHECK(control(53).toggleState == 1);
      TEST_CHECK(control(62).toggleState == 1);
      TEST_CHECK(control(52).toggleState == 1);
    };
    verifyContextSortEntry({{.id = 10, .label = "Open"}}, filemanager::FileMenuContext::File);
    verifyContextSortEntry({{.id = 10, .label = "Open folder"}}, filemanager::FileMenuContext::Folder);
    verifyContextSortEntry({}, filemanager::FileMenuContext::Empty);
    verifyContextSortEntry({}, filemanager::FileMenuContext::Trash);
    const auto sortMenuEntry =
        filemanager::fileSortByMenuEntry(FileDialogSortField::Name, FileDialogSortOrder::Ascending, false, false);
    TEST_CHECK(sortMenuEntry.actionGlyph == "arrows-sort");
    std::vector<ContextMenuControlEntry> toolbarEntries;
    filemanager::appendFileMenuEntries(
        toolbarEntries, filemanager::FileMenuContext::Toolbar, true, false, FileDialogSortField::Name,
        FileDialogSortOrder::Descending, false, true
    );
    TEST_CHECK(toolbarEntries.size() == 3);
    TEST_CHECK(toolbarEntries[0].id == 14 && toolbarEntries[1].id == 50 && toolbarEntries[2].id == 100);

    TEST_CHECK(filemanager::isHiddenFileName(".config"));
    TEST_CHECK(!filemanager::isHiddenFileName("config"));
    TEST_CHECK(!filemanager::isHiddenFileName(""));
    TEST_CHECK(filemanager::fileEntryIconNameAlpha(true, false, false, false) == 0.78F);
    TEST_CHECK(filemanager::fileEntryIconNameAlpha(true, true, false, false) == 1.0F);
    TEST_CHECK(filemanager::fileEntryIconNameAlpha(true, false, true, false) == 1.0F);
    TEST_CHECK(filemanager::fileEntryIconNameAlpha(false, false, false, true) == 0.55F);
    for (const auto* name : {"", ".", "..", "a/b"})
      TEST_CHECK(!filemanager::validFileName(name));
    TEST_CHECK(!filemanager::validFileName(std::string("a\0b", 3)));
    TEST_CHECK(filemanager::validFileName("gamecontrollerdb.txt-backup"));

    nythera::test::ScopedTempDir dropTempDir("nythera-drop-test");
    const auto& root = dropTempDir.path();
    std::filesystem::create_directories(root / "source/child");
    std::filesystem::create_directories(root / "destination");
    std::filesystem::create_directory_symlink(root / "source/child", root / "alias");
    std::ofstream(root / "file") << "fixture";
    const std::vector<std::string> sources{(root / "source").string(), (root / "file").string()};
    TEST_CHECK(filemanager::checkedDropTarget(sources, root / "destination"));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root / "source"));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root / "source/child"));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root / "alias"));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root / "missing"));
    TEST_CHECK(!filemanager::checkedDropTarget(sources, root / "file"));
    TEST_CHECK(!filemanager::checkedDropTarget({}, root / "destination"));
    TEST_CHECK(!filemanager::copyDropByDefault(sources, root / "destination"));
    std::filesystem::create_symlink(root / "file", root / "file-link");
    std::filesystem::create_symlink(root / "missing", root / "broken-link");
    std::filesystem::create_directory_symlink(root / "destination", root / "outside-link");
    for (const auto* name : {"alias", "file-link", "outside-link"}) {
      const auto artwork = queryFileArtwork(root / name, false);
      TEST_CHECK(artwork.symlink && !artwork.broken && !artwork.names.empty());
      TEST_CHECK(artwork.names.back() == (std::string(name) == "file-link" ? "text-x-generic" : "folder"));
    }
    TEST_CHECK(filemanager::checkedDropTarget({(root / "broken-link").string()}, root / "destination"));
    TEST_CHECK(filemanager::checkedDropTarget({(root / "file-link").string()}, root / "destination"));
    TEST_CHECK(!filemanager::checkedDropTarget({(root / "broken-link").string()}, root));
    const auto broken = queryFileArtwork(root / "broken-link", true);
    TEST_CHECK(broken.symlink && broken.broken && broken.names.back() != "folder");
    TEST_CHECK(!queryFileArtwork(root / "file", false).symlink);
    const auto normalFolder = queryFileArtwork(root / "source/child", true);
    TEST_CHECK(queryFileArtwork(root / "alias", false).names == normalFolder.names);

    for (const auto* path :
         {"/", "/home/test", "/home/test/nested/", "trash:", "trash:/", "trash:///", "trash:///folder/"}) {
      const auto crumbs = filemanager::fileBreadcrumbs(path);
      TEST_CHECK(!crumbs.empty());
      for (const auto& crumb : crumbs)
        TEST_CHECK(!crumb.label.empty() && !crumb.path.empty());
      const bool trash = std::string(path).starts_with("trash:");
      TEST_CHECK(crumbs.front().label == (trash ? "Wastebin" : "/"));
      if (trash && std::string(path).find("folder") == std::string::npos)
        TEST_CHECK(crumbs.size() == 1);
    }
    const auto nested = filemanager::fileBreadcrumbs("trash:///folder/child/");
    TEST_CHECK(nested.size() == 3 && nested.back().path == "trash:/folder/child");
    TEST_CHECK(filemanager::fileBreadcrumbs(nested[1].path).size() == 2);

    std::filesystem::create_directory(root / "labels");
    std::filesystem::create_symlink(root / "file", root / "labels/Test\\x20Volume");
    TEST_CHECK(filemanager::filesystemLabel(root / "file", root / "labels") == "Test Volume");
    TEST_CHECK(filemanager::filesystemLabel(root / "missing", root / "labels").empty());
    // F-09: External tool .desktop Exec parsing, terminal flag ordering, quoting, and error handling.
    {
      nythera::test::ScopedTempDir extTempDir("nythera-exttools");
      const auto& extRoot = extTempDir.path();

      // Terminal with "--" separator and stray "%u" + directory containing spaces and '%'.
      const auto termDesktop = extRoot / "custom-term.desktop";
      {
        std::ofstream out(termDesktop);
        out
            << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Name=CustomTerm\n"
            << "Categories=System;TerminalEmulator;\n"
            << "Exec=/usr/bin/true --title \"My Term\" %u -- /bin/sh\n"
            << "X-TerminalArgDir=--working-directory\n";
      }
      auto termApp = filemanager::external_tools::takeApp(g_desktop_app_info_new_from_filename(termDesktop.c_str()));
      TEST_CHECK(termApp != nullptr);
      GError* err = nullptr;
      const std::string spacedDir = "/tmp/dir with spaces 100% ok";
      auto prepared = filemanager::external_tools::launchEntry(
          {termApp, {}}, filemanager::external_tools::Tool::Terminal, spacedDir, &err
      );
      TEST_CHECK(prepared != nullptr && err == nullptr);
      const char* cmd = g_app_info_get_commandline(G_APP_INFO(prepared.get()));
      TEST_CHECK(cmd != nullptr);
      const std::string cmdStr(cmd);
      const auto dirFlagPos = cmdStr.find("--working-directory");
      const auto sepPos = cmdStr.find("'--'");
      TEST_CHECK(dirFlagPos != std::string::npos);
      TEST_CHECK(sepPos != std::string::npos && dirFlagPos < sepPos);
      TEST_CHECK(cmdStr.find("100%%") != std::string::npos);
      TEST_CHECK(cmdStr.find("%u") == std::string::npos);

      // Malformed Exec in a Desktop Action with unclosed quote must fail cleanly with GError.
      const auto badDesktop = extRoot / "bad-exec.desktop";
      {
        std::ofstream out(badDesktop);
        out
            << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Name=BadTerm\n"
            << "Categories=System;TerminalEmulator;\n"
            << "Actions=Broken;\n"
            << "Exec=/usr/bin/true\n\n"
            << "[Desktop Action Broken]\n"
            << "Name=Broken\n"
            << "Exec=/usr/bin/true \"unclosed quote\n";
      }
      auto badApp = filemanager::external_tools::takeApp(g_desktop_app_info_new_from_filename(badDesktop.c_str()));
      TEST_CHECK(badApp != nullptr);
      GError* badErr = nullptr;
      auto badPrepared = filemanager::external_tools::launchEntry(
          {badApp, "Broken"}, filemanager::external_tools::Tool::Terminal, "/tmp", &badErr
      );
      TEST_CHECK(badPrepared == nullptr && badErr != nullptr);
      g_clear_error(&badErr);

      // In-memory GDesktopAppInfo (null filename) must fail cleanly without crashing.
      GKeyFile* memKf = g_key_file_new();
      g_key_file_set_string(memKf, "Desktop Entry", "Type", "Application");
      g_key_file_set_string(memKf, "Desktop Entry", "Name", "MemTerm");
      g_key_file_set_string(memKf, "Desktop Entry", "Exec", "/usr/bin/true");
      auto memApp = filemanager::external_tools::takeApp(g_desktop_app_info_new_from_keyfile(memKf));
      g_key_file_unref(memKf);
      TEST_CHECK(memApp != nullptr);
      GError* memErr = nullptr;
      auto memPrepared = filemanager::external_tools::launchEntry(
          {memApp, {}}, filemanager::external_tools::Tool::Terminal, "/tmp", &memErr
      );
      TEST_CHECK(memPrepared == nullptr && memErr != nullptr);
      g_clear_error(&memErr);

      // Compare tool without field code appends %F cleanly.
      const auto cmpDesktop = extRoot / "meld.desktop";
      {
        std::ofstream out(cmpDesktop);
        out
            << "[Desktop Entry]\n"
            << "Type=Application\n"
            << "Name=Meld\n"
            << "Exec=/usr/bin/true --diff\n";
      }
      auto cmpApp = filemanager::external_tools::takeApp(g_desktop_app_info_new_from_filename(cmpDesktop.c_str()));
      TEST_CHECK(cmpApp != nullptr);
      GError* cmpErr = nullptr;
      auto cmpPrepared = filemanager::external_tools::launchEntry(
          {cmpApp, {}}, filemanager::external_tools::Tool::Compare, "/tmp", &cmpErr
      );
      TEST_CHECK(cmpPrepared != nullptr && cmpErr == nullptr);
      const std::string cmpCmd(g_app_info_get_commandline(G_APP_INFO(cmpPrepared.get())));
      TEST_CHECK(cmpCmd.ends_with(" %F"));
    }
  });
}
