#include "core/build_info.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "filemanager/file_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_background_artwork.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  class StubRenderer final : public Renderer {
  public:
    TextMetrics measureText(
        std::string_view text, float fontSize, const TextFont&, float maxWidth, int maxLines, TextAlign, TextEllipsize,
        bool
    ) override {
      constexpr float kAdvance = 8.0F;
      const float natural = static_cast<float>(text.size()) * kAdvance;
      float width = natural;
      int lineCount = text.empty() ? 0 : 1;
      if (maxWidth > 0.0F && natural > maxWidth) {
        const float perLine = std::floor(maxWidth / kAdvance) * kAdvance;
        lineCount = perLine > 0.0F ? static_cast<int>(std::ceil(natural / perLine)) : 1;
        if (maxLines > 0)
          lineCount = std::min(lineCount, maxLines);
        width = maxWidth;
      }
      return TextMetrics{
          .width = width,
          .right = width,
          .bottom = fontSize * static_cast<float>(lineCount),
          .lineCount = lineCount,
      };
    }

    TextMetrics measureFont(float fontSize, const TextFont&) override { return TextMetrics{.bottom = fontSize}; }
    void measureTextCursorStops(
        std::string_view, float, const std::vector<std::size_t>&, std::vector<float>&, const TextFont&
    ) override {}
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>&, const TextFont&
    ) override {}
    TextMetrics measureGlyph(char32_t, float) override { return {.width = 16.0F, .right = 16.0F, .bottom = 16.0F}; }
    TextureManager& textureManager() override { std::abort(); }
    [[nodiscard]] float renderScale() const noexcept override { return 1.0F; }
  };

  const RectNode* backgroundRect(const Flex* flex) {
    if (flex == nullptr)
      return nullptr;
    for (const auto& child : flex->children()) {
      if (const auto* rect = dynamic_cast<const RectNode*>(child.get()))
        return rect;
    }
    return nullptr;
  }

  void collectLabels(const Node* node, std::vector<const Label*>& out) {
    if (node == nullptr)
      return;
    if (const auto* label = dynamic_cast<const Label*>(node))
      out.push_back(label);
    for (const auto& child : node->children())
      collectLabels(child.get(), out);
  }

  void collectLabelTexts(const Node* node, std::vector<std::string>& out) {
    std::vector<const Label*> labels;
    collectLabels(node, labels);
    for (const auto* label : labels)
      out.push_back(label->text());
  }

  bool containsText(const std::vector<std::string>& texts, std::string_view needle) {
    return std::ranges::any_of(texts, [needle](const std::string& text) { return text == needle; });
  }

  std::vector<ContextMenuControlEntry> interactiveOnly(const std::vector<ContextMenuControlEntry>& entries) {
    std::vector<ContextMenuControlEntry> out;
    for (const auto& entry : entries) {
      if (!entry.separator && !entry.header)
        out.push_back(entry);
    }
    return out;
  }

  void verifyHelpEntries(const std::vector<ContextMenuControlEntry>& entries) {
    const auto items = interactiveOnly(entries);
    TEST_CHECK(items.size() == 4);

    constexpr std::array<int, 4> expectedIds{
        filemanager::menu_id::kKeyboardShortcuts,
        filemanager::menu_id::kReportBug,
        filemanager::menu_id::kGitHub,
        filemanager::menu_id::kAboutFiles,
    };
    constexpr std::array<std::string_view, 4> expectedLabels{
        "Keyboard Shortcuts",
        "Report a Bug\xE2\x80\xA6",
        "Nythera Files on GitHub",
        "About Nythera Files",
    };
    constexpr std::array<std::string_view, 4> expectedGlyphs{
        "keyboard",
        "bug",
        "brand-github",
        "info-circle",
    };

    for (std::size_t i = 0; i < items.size(); ++i) {
      TEST_CHECK(items[i].id == expectedIds[i]);
      TEST_CHECK(items[i].label == expectedLabels[i]);
      TEST_CHECK(items[i].enabled);
      TEST_CHECK(items[i].actionGlyph == expectedGlyphs[i]);
    }
  }

  Palette makeTestPalette(Color primary, Color surfaceLow, Color surfaceContainer, Color onSurface) {
    Palette p{};
    p.primary = primary;
    p.onPrimary = rgba(1.0F, 1.0F, 1.0F, 1.0F);
    p.primaryContainer = primary;
    p.onPrimaryContainer = onSurface;
    p.secondary = primary;
    p.onSecondary = onSurface;
    p.secondaryContainer = surfaceContainer;
    p.onSecondaryContainer = onSurface;
    p.tertiary = primary;
    p.onTertiary = onSurface;
    p.tertiaryContainer = surfaceContainer;
    p.onTertiaryContainer = onSurface;
    p.error = rgba(0.9F, 0.2F, 0.2F, 1.0F);
    p.onError = rgba(1.0F, 1.0F, 1.0F, 1.0F);
    p.errorContainer = surfaceContainer;
    p.onErrorContainer = onSurface;
    p.surface = surfaceLow;
    p.onSurface = onSurface;
    p.onSurfaceVariant = rgba(onSurface.r * 0.7F, onSurface.g * 0.7F, onSurface.b * 0.7F, 1.0F);
    p.surfaceContainerLow = surfaceLow;
    p.surfaceContainer = surfaceContainer;
    p.surfaceContainerHigh = surfaceContainer;
    p.surfaceContainerHighest = surfaceContainer;
    p.outlineVariant = rgba(0.4F, 0.4F, 0.45F, 1.0F);
    p.shadow = rgba(0.0F, 0.0F, 0.0F, 1.0F);
    p.scrim = rgba(0.0F, 0.0F, 0.0F, 0.6F);
    p.hover = surfaceContainer;
    p.onHover = onSurface;
    return p;
  }

} // namespace

class FileManagerTestAccess {
public:
  static void run(const std::filesystem::path& browseDir) {
    const Palette initialPalette = makeTestPalette(
        rgba(0.85F, 0.12F, 0.32F, 1.0F), rgba(0.08F, 0.08F, 0.11F, 1.0F), rgba(0.14F, 0.14F, 0.19F, 1.0F),
        rgba(0.95F, 0.95F, 0.97F, 1.0F)
    );
    setPalette(initialPalette);

    FileManager manager(browseDir.string());
    StubRenderer renderer;

    // 1. Help entries in both normal and Wastebin browsing states.
    verifyHelpEntries(manager.helpMenuEntries());
    manager.activeSession().m_browsingTrash = true;
    verifyHelpEntries(manager.helpMenuEntries());
    manager.activeSession().m_browsingTrash = false;

    // 2. Normal menu bar and collapsed hamburger menu exposure.
    Flex appBar;
    manager.m_menus.attachMenuBarUi(appBar, 32.0F);
    TEST_CHECK(manager.m_menus.m_applicationMenuButtons[filemanager::menu_id::kHelpMenuIndex] != nullptr);
    TEST_CHECK(manager.m_menus.m_applicationMenuButtons[filemanager::menu_id::kHelpMenuIndex]->label() != nullptr);
    TEST_CHECK(
        manager.m_menus.m_applicationMenuButtons[filemanager::menu_id::kHelpMenuIndex]->label()->text() == "Help"
    );
    TEST_CHECK(manager.m_menus.m_appMenuItems != nullptr);
    TEST_CHECK(manager.m_menus.m_compactMenuButton != nullptr);

    manager.m_menus.syncResponsiveMenuBar(1120);
    TEST_CHECK(manager.m_menus.m_appMenuItems->visible());
    TEST_CHECK(!manager.m_menus.m_compactMenuButton->visible());

    const auto directHelp = manager.m_menus.applicationMenuEntries(filemanager::menu_id::kHelpMenuIndex);
    TEST_CHECK(directHelp.has_value());
    verifyHelpEntries(*directHelp);

    const auto directFile = manager.m_menus.applicationMenuEntries(0);
    TEST_CHECK(directFile.has_value());
    TEST_CHECK(std::ranges::none_of(*directFile, [](const auto& entry) {
      return entry.id == filemanager::menu_id::kCompactHelpMenu;
    }));

    // Narrow width collapse exposes Help in the hamburger menu.
    manager.m_menus.syncResponsiveMenuBar(500);
    TEST_CHECK(!manager.m_menus.m_appMenuItems->visible());
    TEST_CHECK(manager.m_menus.m_compactMenuButton->visible());
    const auto collapsedByWidth = manager.m_menus.applicationMenuEntries(0);
    TEST_CHECK(collapsedByWidth.has_value());
    const auto compactHelpIt = std::ranges::find_if(*collapsedByWidth, [](const auto& entry) {
      return entry.id == filemanager::menu_id::kCompactHelpMenu;
    });
    TEST_CHECK(compactHelpIt != collapsedByWidth->end());
    TEST_CHECK(compactHelpIt->label == "Help");
    TEST_CHECK(compactHelpIt->hasSubmenu);
    verifyHelpEntries(compactHelpIt->submenuEntries);
    for (const int expectedCompactId :
         {filemanager::menu_id::kCompactEditMenu, filemanager::menu_id::kCompactViewMenu,
          filemanager::menu_id::kCompactToolsMenu, filemanager::menu_id::kCompactHelpMenu}) {
      TEST_CHECK(std::ranges::any_of(*collapsedByWidth, [expectedCompactId](const auto& entry) {
        return entry.id == expectedCompactId;
      }));
    }

    // Menu bar preference toggle collapse at wide width also exposes Help in the hamburger menu.
    manager.m_menus.syncResponsiveMenuBar(1120);
    TEST_CHECK(manager.m_menus.m_showApplicationMenuBar);
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kShowMenuBar});
    TEST_CHECK(!manager.m_menus.m_showApplicationMenuBar);
    manager.m_menus.syncResponsiveMenuBar(1120);
    TEST_CHECK(!manager.m_menus.m_appMenuItems->visible());
    TEST_CHECK(manager.m_menus.m_compactMenuButton->visible());
    const auto collapsedByPref = manager.m_menus.applicationMenuEntries(0);
    TEST_CHECK(collapsedByPref.has_value());
    const auto compactPrefHelp = std::ranges::find_if(*collapsedByPref, [](const auto& entry) {
      return entry.id == filemanager::menu_id::kCompactHelpMenu;
    });
    TEST_CHECK(compactPrefHelp != collapsedByPref->end());
    verifyHelpEntries(compactPrefHelp->submenuEntries);

    // Restore preference and verify existing View/Edit actions still work.
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kShowMenuBar});
    TEST_CHECK(manager.m_menus.m_showApplicationMenuBar);
    manager.m_menus.syncResponsiveMenuBar(1120);
    TEST_CHECK(manager.m_menus.m_appMenuItems->visible());

    TEST_CHECK(!manager.m_listHeader.gridMode());
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kGridView});
    TEST_CHECK(manager.m_listHeader.gridMode());
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kListView});
    TEST_CHECK(!manager.m_listHeader.gridMode());

    manager.m_menus.detachUi();

    // 3. External URL actions (Report a Bug & GitHub) + failure status handling.
    std::vector<std::string> openedUrls;
    bool openShouldSucceed = true;
    manager.m_helpDialog.m_callbacks.openUrl = [&](const std::string& url) {
      openedUrls.push_back(url);
      return openShouldSucceed;
    };

    manager.activateContextMenuEntry({.id = filemanager::menu_id::kReportBug});
    TEST_CHECK(openedUrls.size() == 1);
    TEST_CHECK(openedUrls.back() == "https://github.com/Nythera-Dev/Nythera/issues");

    manager.activateContextMenuEntry({.id = filemanager::menu_id::kGitHub});
    TEST_CHECK(openedUrls.size() == 2);
    TEST_CHECK(openedUrls.back() == "https://github.com/Nythera-Dev/Nythera");

    openShouldSucceed = false;
    manager.activateContextMenuEntry({.id = filemanager::menu_id::kReportBug});
    TEST_CHECK(openedUrls.size() == 3);
    TEST_CHECK(manager.m_statusMessage == "Could not open link in default browser");
    openShouldSucceed = true;

    // 3b. Ctrl+Shift+N creates a New Folder on both shifted XKB_KEY_N and lowercase XKB_KEY_n.
    {
      const auto waitForPredicate = [](auto&& predicate) {
        bool timedOut = false;
        const guint timer = g_timeout_add(
            5000,
            [](gpointer data) -> gboolean {
              *static_cast<bool*>(data) = true;
              return G_SOURCE_REMOVE;
            },
            &timedOut
        );
        while (!predicate() && !timedOut)
          g_main_context_iteration(nullptr, TRUE);
        if (!timedOut)
          g_source_remove(timer);
        TEST_CHECK(!timedOut && predicate());
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
      };

      auto& tab = manager.activeTab();
      auto& session = tab.session;
      manager.navigate(browseDir, false);
      waitForPredicate([&] { return !session.m_loading && manager.currentDirectoryWritable(); });
      session.closeMonitor();

      manager.handleWindowKeyEvent(
          KeyboardEvent{.sym = XKB_KEY_N, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
      );
      waitForPredicate([&] { return !manager.m_operations.active() && !session.m_loading; });
      session.closeMonitor();
      const auto firstFolder = browseDir / "New Folder";
      TEST_CHECK(std::filesystem::is_directory(firstFolder));
      TEST_CHECK(tab.selectedPath && *tab.selectedPath == firstFolder);
      TEST_CHECK(manager.m_pendingInlineRenamePath && *manager.m_pendingInlineRenamePath == firstFolder);

      manager.m_pendingInlineRenamePath.reset();
      manager.handleWindowKeyEvent(
          KeyboardEvent{.sym = XKB_KEY_n, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
      );
      waitForPredicate([&] { return !manager.m_operations.active() && !session.m_loading; });
      session.closeMonitor();
      const auto secondFolder = browseDir / "New Folder 2";
      TEST_CHECK(std::filesystem::is_directory(secondFolder));
      TEST_CHECK(tab.selectedPath && *tab.selectedPath == secondFolder);
      TEST_CHECK(manager.m_pendingInlineRenamePath && *manager.m_pendingInlineRenamePath == secondFolder);

      manager.m_pendingInlineRenamePath.reset();
      manager.handleWindowKeyEvent(
          KeyboardEvent{.sym = XKB_KEY_N, .modifiers = KeyMod::Ctrl | KeyMod::Shift | KeyMod::Alt, .pressed = true}
      );
      TEST_CHECK(!manager.m_operations.active());
      TEST_CHECK(!std::filesystem::exists(browseDir / "New Folder 3"));

      session.m_browsingTrash = true;
      manager.handleWindowKeyEvent(
          KeyboardEvent{.sym = XKB_KEY_N, .modifiers = KeyMod::Ctrl | KeyMod::Shift, .pressed = true}
      );
      TEST_CHECK(!manager.m_operations.active());
      TEST_CHECK(!std::filesystem::exists(browseDir / "New Folder 3"));
      session.m_browsingTrash = false;
    }

    // 4. Keyboard Shortcuts dialog tree, scrolling, verified shortcuts, and theme updates.
    {
      auto& modal = manager.m_modalDialog;
      modal.m_title = "Keyboard Shortcuts";
      modal.m_message.clear();
      modal.m_choices = {{"Close", 0}};
      modal.m_customContent = manager.m_helpDialog.makeShortcutsCustomContent();
      modal.buildDialog(540.0F);

      TEST_CHECK(modal.m_topAccent != nullptr);
      TEST_CHECK(modal.m_cornerAccent != nullptr);
      TEST_CHECK(modal.m_deleteWarning == nullptr);
      TEST_CHECK(modal.m_divider != nullptr);
      TEST_CHECK(modal.m_footer != nullptr);
      TEST_CHECK(modal.m_spacer != nullptr && !modal.m_spacer->visible() && !modal.m_spacer->participatesInLayout());
      TEST_CHECK(manager.m_helpDialog.m_shortcutsScroll != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_shortcutsClose != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_shortcutsClose->label() != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_shortcutsClose->label()->text() == "Close");
      TEST_CHECK(modal.m_cards.size() == filemanager::HelpDialogController::shortcutGroups().size());

      const auto* rootBg = backgroundRect(modal.m_root.get());
      TEST_CHECK(rootBg != nullptr);
      TEST_CHECK(rootBg->style().corners.tl == CornerShape::Chamfered);
      TEST_CHECK(rootBg->style().corners.br == CornerShape::Chamfered);
      TEST_CHECK(rootBg->style().border == initialPalette.primary);

      std::vector<std::string> texts;
      collectLabelTexts(modal.m_root.get(), texts);
      TEST_CHECK(containsText(texts, "Keyboard Shortcuts"));
      TEST_CHECK(containsText(texts, "Close"));
      for (const auto& group : filemanager::HelpDialogController::shortcutGroups()) {
        TEST_CHECK(containsText(texts, group.title));
        for (const auto& entry : group.entries) {
          TEST_CHECK(containsText(texts, entry.action));
          TEST_CHECK(containsText(texts, entry.shortcut));
        }
      }
      TEST_CHECK(containsText(texts, "New Folder"));
      TEST_CHECK(containsText(texts, "Ctrl+Shift+N"));
      TEST_CHECK(containsText(texts, "F5 / Ctrl+R"));
      // Unimplemented shortcuts must not be advertised.
      TEST_CHECK(!containsText(texts, "Ctrl+F"));

      // Constrain dialog height to verify ScrollView scrollability and keyboard scrolling.
      modal.m_root->setSize(540.0F, 320.0F);
      modal.m_root->layout(renderer);
      auto* scroll = manager.m_helpDialog.m_shortcutsScroll;
      TEST_CHECK(scroll->scrollable());
      TEST_CHECK(scroll->maxScrollOffset() > 0.0F);
      TEST_CHECK(scroll->scrollOffset() == 0.0F);

      TEST_CHECK(modal.m_customContent->onKeyEvent(KeyboardEvent{.sym = XKB_KEY_Down, .pressed = true}));
      TEST_CHECK(scroll->scrollOffset() > 0.0F);
      const float afterDown = scroll->scrollOffset();

      TEST_CHECK(modal.m_customContent->onKeyEvent(KeyboardEvent{.sym = XKB_KEY_Page_Down, .pressed = true}));
      TEST_CHECK(scroll->scrollOffset() >= afterDown);

      TEST_CHECK(modal.m_customContent->onKeyEvent(KeyboardEvent{.sym = XKB_KEY_End, .pressed = true}));
      TEST_CHECK(std::abs(scroll->scrollOffset() - scroll->maxScrollOffset()) < 0.01F);

      TEST_CHECK(modal.m_customContent->onKeyEvent(KeyboardEvent{.sym = XKB_KEY_Home, .pressed = true}));
      TEST_CHECK(scroll->scrollOffset() == 0.0F);

      // Theme reload updates root and all registered shortcut group cards.
      const Palette updatedPalette = makeTestPalette(
          rgba(0.18F, 0.72F, 0.85F, 1.0F), rgba(0.10F, 0.12F, 0.16F, 1.0F), rgba(0.18F, 0.22F, 0.28F, 1.0F),
          rgba(0.98F, 0.98F, 1.0F, 1.0F)
      );
      setPalette(updatedPalette);
      modal.updateTheme();
      const auto* updatedRootBg = backgroundRect(modal.m_root.get());
      TEST_CHECK(updatedRootBg != nullptr);
      TEST_CHECK(updatedRootBg->style().border == updatedPalette.primary);
      TEST_CHECK(std::abs(updatedRootBg->style().fill.r - updatedPalette.surfaceContainerLow.r) < 0.01F);
      for (const auto* card : modal.m_cards) {
        const auto* cardBg = backgroundRect(card);
        TEST_CHECK(cardBg != nullptr);
        TEST_CHECK(std::abs(cardBg->style().fill.r - updatedPalette.surfaceContainer.r) < 0.01F);
        TEST_CHECK(cardBg->style().border == updatedPalette.outlineVariant);
      }

      modal.m_customContent.reset();
      modal.m_cards.clear();
      modal.m_root.reset();
      manager.m_helpDialog.dismiss();
    }

    // 5. About Nythera Files dialog metadata, branding, icon, repo link click, and theme updates.
    {
      setPalette(initialPalette);
      const auto info = filemanager::HelpDialogController::aboutInfo();
      TEST_CHECK(info.appName == "Nythera Files");
      TEST_CHECK(info.version == nythera::build_info::displayVersion());
      TEST_CHECK(!info.description.empty());
      TEST_CHECK(info.licence == "MIT License");
      TEST_CHECK(info.copyright == "Copyright \xC2\xA9 2026 Kangy");
      TEST_CHECK(info.credits == "Incorporates Noctalia-derived rendering and UI infrastructure.");
      TEST_CHECK(info.creditsCopyright == "Copyright \xC2\xA9 2026 noctalia-dev");
      TEST_CHECK(info.repositoryUrl == "https://github.com/Nythera-Dev/Nythera");
      TEST_CHECK(info.iconPath.string().ends_with("concepts/nythera-files-icon.png"));
      TEST_CHECK(std::filesystem::is_regular_file(info.iconPath));
      TEST_CHECK(info.licencePath.filename() == "LICENSE");
      TEST_CHECK(std::filesystem::is_regular_file(info.licencePath));
      {
        std::ifstream licenceFile(info.licencePath);
        TEST_CHECK(licenceFile.is_open());
        std::ostringstream buffer;
        buffer << licenceFile.rdbuf();
        const std::string licenceContents = buffer.str();
        TEST_CHECK(licenceContents.find("MIT License") != std::string::npos);
        TEST_CHECK(licenceContents.find("Copyright (c) 2026 Kangy") != std::string::npos);
        TEST_CHECK(licenceContents.find("Copyright (c) 2026 noctalia-dev") != std::string::npos);
        TEST_CHECK(licenceContents.find("Tabler Icons") != std::string::npos);
      }

      auto& modal = manager.m_modalDialog;
      modal.m_title = "About Nythera Files";
      modal.m_message.clear();
      modal.m_choices = {{"Close", 0}};
      modal.m_customContent = manager.m_helpDialog.makeAboutCustomContent();
      modal.buildDialog(500.0F);

      TEST_CHECK(modal.m_topAccent != nullptr);
      TEST_CHECK(modal.m_cornerAccent != nullptr);
      TEST_CHECK(modal.m_deleteWarning == nullptr);
      TEST_CHECK(modal.m_divider != nullptr);
      TEST_CHECK(modal.m_footer != nullptr);
      TEST_CHECK(modal.m_cards.size() == 1);
      TEST_CHECK(manager.m_helpDialog.m_aboutIcon != nullptr);
      TEST_CHECK(std::abs(manager.m_helpDialog.m_aboutIcon->width() - 84.0F) < 0.01F);
      TEST_CHECK(std::abs(manager.m_helpDialog.m_aboutIcon->height() - 63.0F) < 0.01F);
      TEST_CHECK(manager.m_helpDialog.m_repositoryButton != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_aboutClose != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_aboutClose->label() != nullptr);
      TEST_CHECK(manager.m_helpDialog.m_aboutClose->label()->text() == "Close");

      std::vector<const Label*> labels;
      collectLabels(modal.m_root.get(), labels);
      bool foundNytheraDisplay = false;
      bool foundFilesDisplay = false;
      std::vector<std::string> texts;
      for (const auto* label : labels) {
        texts.push_back(label->text());
        if (label->text() == "NYTHERA" && label->font().directDisplay)
          foundNytheraDisplay = true;
        if (label->text() == "FILES" && label->font().directDisplay)
          foundFilesDisplay = true;
      }
      TEST_CHECK(foundNytheraDisplay);
      TEST_CHECK(foundFilesDisplay);
      TEST_CHECK(containsText(texts, "About Nythera Files"));
      TEST_CHECK(containsText(texts, info.description));
      TEST_CHECK(containsText(texts, info.version));
      TEST_CHECK(containsText(texts, "Licence"));
      TEST_CHECK(containsText(texts, info.licence));
      TEST_CHECK(containsText(texts, info.copyright));
      TEST_CHECK(containsText(texts, "Credits"));
      TEST_CHECK(containsText(texts, info.credits));
      TEST_CHECK(containsText(texts, info.creditsCopyright));
      TEST_CHECK(containsText(texts, "Repository"));
      TEST_CHECK(containsText(texts, info.repositoryUrl));

      // Layout and click the repository link button via its InputArea.
      LayoutConstraints aboutConstraints;
      aboutConstraints.setExactWidth(500.0F);
      const auto measured = modal.m_root->measure(renderer, aboutConstraints);
      modal.m_root->setSize(500.0F, measured.height);
      modal.m_root->layout(renderer);
      TEST_CHECK(measured.height > 180.0F);
      auto* repoButton = manager.m_helpDialog.m_repositoryButton;
      auto* closeButton = manager.m_helpDialog.m_aboutClose;
      float repoX = 0.0F;
      float repoY = 0.0F;
      Node::absolutePosition(repoButton, repoX, repoY);
      float closeX = 0.0F;
      float closeY = 0.0F;
      Node::absolutePosition(closeButton, closeX, closeY);
      TEST_CHECK(repoY > 0.0F && repoY + repoButton->height() < closeY);
      TEST_CHECK(closeY + closeButton->height() <= measured.height);
      TEST_CHECK(repoButton->inputArea() != nullptr);
      TEST_CHECK(repoButton->inputArea()->enabled());
      const std::size_t beforeRepoClick = openedUrls.size();
      repoButton->inputArea()->dispatchPress(5.0F, 5.0F, BTN_LEFT, true);
      repoButton->inputArea()->dispatchPress(5.0F, 5.0F, BTN_LEFT, false);
      TEST_CHECK(openedUrls.size() == beforeRepoClick + 1);
      TEST_CHECK(openedUrls.back() == "https://github.com/Nythera-Dev/Nythera");

      const Palette secondPalette = makeTestPalette(
          rgba(0.24F, 0.80F, 0.45F, 1.0F), rgba(0.09F, 0.11F, 0.10F, 1.0F), rgba(0.15F, 0.19F, 0.17F, 1.0F),
          rgba(0.94F, 0.96F, 0.95F, 1.0F)
      );
      setPalette(secondPalette);
      modal.updateTheme();
      const auto* cardBg = backgroundRect(modal.m_cards.front());
      TEST_CHECK(cardBg != nullptr);
      TEST_CHECK(std::abs(cardBg->style().fill.r - secondPalette.surfaceContainer.r) < 0.01F);

      modal.m_customContent.reset();
      modal.m_cards.clear();
      modal.m_root.reset();
      manager.m_helpDialog.dismiss();
    }
  }
};

int main() {
  return nythera::test::runTest([] {
    paths::selectFilesAssets();
    nythera::test::ScopedTestEnv env({.prefix = "nythera-fm-help-menu"});
    const auto browseDir = env.work() / "browse";
    std::filesystem::create_directory(browseDir);
    std::ofstream(browseDir / "sample.txt") << "hello";
    FileManagerTestAccess::run(browseDir);
  });
}
