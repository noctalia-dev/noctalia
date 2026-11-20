#include "filemanager/external_tools.h"
#include "filemanager/file_breadcrumbs.h"
#include "filemanager/file_creation.h"
#include "filemanager/file_interaction.h"
#include "filemanager/file_view_menu.h"
#include "filemanager/place_metadata.h"
#include "ui/dialogs/file_artwork.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>

int main() {
  constexpr auto testHome = "/home/example";
  assert(filemanager::expandHomePath("~", testHome) == testHome);
  assert(filemanager::expandHomePath("~/", testHome) == testHome);
  assert(filemanager::expandHomePath("~/.config", testHome) == "/home/example/.config");
  assert(filemanager::expandHomePath("~/Documents", testHome) == "/home/example/Documents");
  assert(filemanager::expandHomePath("~/Documents", "/") == "/Documents");
  for (const auto* unchanged :
       {"/mnt/Media", "../folder/./item", "folder/~", "~other/.config", "$HOME", "file:///tmp/~/file", "trash:/", ""})
    assert(filemanager::expandHomePath(unchanged, testHome) == unchanged);

  auto creationTemplate = (std::filesystem::temp_directory_path() / "nythera-file-creation-test-XXXXXX").string();
  const auto* createdDirectory = ::mkdtemp(creationTemplate.data());
  assert(createdDirectory != nullptr);
  const std::filesystem::path creationRoot(createdDirectory);
  std::error_code creationError;
  const auto emptyFile = filemanager::creationDestination(creationRoot, "notes", false, creationError);
  assert(emptyFile && *emptyFile == creationRoot / "notes" && !creationError);
  std::ofstream(*emptyFile).put('x');
  const auto collision = filemanager::creationDestination(creationRoot, "notes", false, creationError);
  assert(collision && *collision == creationRoot / "notes 2" && !creationError);
  const auto textFile = filemanager::creationDestination(creationRoot, "readme.TXT", true, creationError);
  assert(textFile && *textFile == creationRoot / "readme.TXT" && !creationError);
  const auto textFileWithExtension = filemanager::creationDestination(creationRoot, "readme", true, creationError);
  assert(textFileWithExtension && *textFileWithExtension == creationRoot / "readme.txt" && !creationError);
  assert(!filemanager::creationDestination(creationRoot, "../escape", false, creationError) && creationError);
  std::filesystem::remove_all(creationRoot, creationError);

  const auto verifyMenuGroups = [](std::vector<ContextMenuControlEntry> entries, filemanager::FileMenuContext context,
                                   const std::vector<int>& expectedIds) {
    filemanager::groupFileMenuEntries(entries, context);
    std::vector<int> actualIds;
    bool previousSeparator = true;
    for (const auto& entry : entries) {
      assert(!entry.header);
      if (entry.separator) {
        assert(!previousSeparator);
        previousSeparator = true;
      } else {
        actualIds.push_back(entry.id);
        previousSeparator = false;
      }
    }
    assert(!previousSeparator);
    assert(actualIds == expectedIds);
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
  assert(optionalEntries.size() == 1 && optionalEntries.front().id == 21); // Empty optional groups leave no rules.
  std::vector<ContextMenuControlEntry> disabledEntries{{.id = 4, .enabled = false}, {.id = 10}};
  filemanager::groupFileMenuEntries(disabledEntries, filemanager::FileMenuContext::File);
  assert(disabledEntries.size() == 3 && disabledEntries.front().id == 10 && !disabledEntries.back().enabled);

  assert(filemanager::locationEditCancelIsCurrent(true, 3, 3));
  assert(!filemanager::locationEditCancelIsCurrent(false, 3, 3));
  assert(!filemanager::locationEditCancelIsCurrent(true, 4, 3));
  assert(filemanager::pointerOutsideRect(9, 20, 10, 10, 40, 20));
  assert(!filemanager::pointerOutsideRect(10, 10, 10, 10, 40, 20));
  assert(!filemanager::pointerOutsideRect(49, 29, 10, 10, 40, 20));

  const auto verifyContextSortEntry = [](std::vector<ContextMenuControlEntry> entries,
                                         filemanager::FileMenuContext context) {
    filemanager::appendFileMenuEntries(
        entries, context, true, false, FileDialogSortField::Name, FileDialogSortOrder::Descending, false, true
    );
    assert(entries.back().id == 100 && entries.back().hasSubmenu);
    assert(std::ranges::count(entries, 100, &ContextMenuControlEntry::id) == 1);
    const bool hasViewActions = context == filemanager::FileMenuContext::Empty;
    assert((std::ranges::find(entries, 14, &ContextMenuControlEntry::id) != entries.end()) == hasViewActions);
    assert((std::ranges::find(entries, 50, &ContextMenuControlEntry::id) != entries.end()) == hasViewActions);
    const auto& sortEntries = entries.back().submenuEntries;
    assert(sortEntries.size() == 10);
    const auto control = [&sortEntries](int id) -> const ContextMenuControlEntry& {
      const auto found = std::ranges::find(sortEntries, id, &ContextMenuControlEntry::id);
      assert(found != sortEntries.end());
      return *found;
    };
    assert(control(53).toggleState == 1);
    assert(control(62).toggleState == 1);
    assert(control(52).toggleState == 1);
  };
  verifyContextSortEntry({{.id = 10, .label = "Open"}}, filemanager::FileMenuContext::File);
  verifyContextSortEntry({{.id = 10, .label = "Open folder"}}, filemanager::FileMenuContext::Folder);
  verifyContextSortEntry({}, filemanager::FileMenuContext::Empty);
  verifyContextSortEntry({}, filemanager::FileMenuContext::Trash);
  const auto sortMenuEntry =
      filemanager::fileSortByMenuEntry(FileDialogSortField::Name, FileDialogSortOrder::Ascending, false, false);
  assert(sortMenuEntry.actionGlyph == "arrows-sort");
  std::vector<ContextMenuControlEntry> toolbarEntries;
  filemanager::appendFileMenuEntries(
      toolbarEntries, filemanager::FileMenuContext::Toolbar, true, false, FileDialogSortField::Name,
      FileDialogSortOrder::Descending, false, true
  );
  assert(toolbarEntries.size() == 3);
  assert(toolbarEntries[0].id == 14 && toolbarEntries[1].id == 50 && toolbarEntries[2].id == 100);

  assert(filemanager::isHiddenFileName(".config"));
  assert(!filemanager::isHiddenFileName("config"));
  assert(!filemanager::isHiddenFileName(""));
  assert(filemanager::fileEntryIconNameAlpha(true, false, false, false) == 0.78F);
  assert(filemanager::fileEntryIconNameAlpha(true, true, false, false) == 1.0F);
  assert(filemanager::fileEntryIconNameAlpha(true, false, true, false) == 1.0F);
  assert(filemanager::fileEntryIconNameAlpha(false, false, false, true) == 0.55F);
  for (const auto* name : {"", ".", "..", "a/b"})
    assert(!filemanager::validFileName(name));
  assert(!filemanager::validFileName(std::string("a\0b", 3)));
  assert(filemanager::validFileName("gamecontrollerdb.txt-backup"));

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
    assert(crumbs.front().label == (trash ? "Wastebin" : "/"));
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
  // F-09: External tool .desktop Exec parsing, terminal flag ordering, quoting, and error handling.
  {
    char* extRaw = g_dir_make_tmp("nythera-exttools-XXXXXX", nullptr);
    assert(extRaw != nullptr);
    const std::filesystem::path extRoot(extRaw);
    g_free(extRaw);

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
    assert(termApp != nullptr);
    GError* err = nullptr;
    const std::string spacedDir = "/tmp/dir with spaces 100% ok";
    auto prepared = filemanager::external_tools::launchEntry(
        {termApp, {}}, filemanager::external_tools::Tool::Terminal, spacedDir, &err
    );
    assert(prepared != nullptr && err == nullptr);
    const char* cmd = g_app_info_get_commandline(G_APP_INFO(prepared.get()));
    assert(cmd != nullptr);
    const std::string cmdStr(cmd);
    const auto dirFlagPos = cmdStr.find("--working-directory");
    const auto sepPos = cmdStr.find("'--'");
    assert(dirFlagPos != std::string::npos);
    assert(sepPos != std::string::npos && dirFlagPos < sepPos);
    assert(cmdStr.find("100%%") != std::string::npos);
    assert(cmdStr.find("%u") == std::string::npos);

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
    assert(badApp != nullptr);
    GError* badErr = nullptr;
    auto badPrepared = filemanager::external_tools::launchEntry(
        {badApp, "Broken"}, filemanager::external_tools::Tool::Terminal, "/tmp", &badErr
    );
    assert(badPrepared == nullptr && badErr != nullptr);
    g_clear_error(&badErr);

    // In-memory GDesktopAppInfo (null filename) must fail cleanly without crashing.
    GKeyFile* memKf = g_key_file_new();
    g_key_file_set_string(memKf, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(memKf, "Desktop Entry", "Name", "MemTerm");
    g_key_file_set_string(memKf, "Desktop Entry", "Exec", "/usr/bin/true");
    auto memApp = filemanager::external_tools::takeApp(g_desktop_app_info_new_from_keyfile(memKf));
    g_key_file_unref(memKf);
    assert(memApp != nullptr);
    GError* memErr = nullptr;
    auto memPrepared = filemanager::external_tools::launchEntry(
        {memApp, {}}, filemanager::external_tools::Tool::Terminal, "/tmp", &memErr
    );
    assert(memPrepared == nullptr && memErr != nullptr);
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
    assert(cmpApp != nullptr);
    GError* cmpErr = nullptr;
    auto cmpPrepared = filemanager::external_tools::launchEntry(
        {cmpApp, {}}, filemanager::external_tools::Tool::Compare, "/tmp", &cmpErr
    );
    assert(cmpPrepared != nullptr && cmpErr == nullptr);
    const std::string cmpCmd(g_app_info_get_commandline(G_APP_INFO(cmpPrepared.get())));
    assert(cmpCmd.ends_with(" %F"));

    std::filesystem::remove_all(extRoot);
  }
}
