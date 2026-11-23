#include "core/text_clipboard.h"
#include "filemanager/file_manager.h"
#include "filemanager/properties_window.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "test_isolation.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/controls/select.h"
#include "ui/controls/select_popup_context.h"
#include "ui/controls/virtual_grid_view.h"
#include "ui/dialogs/file_entry_row.h"
#include "ui/dialogs/file_entry_tile.h"

#include <array>
#include <chrono>
#include <fstream>
#include <linux/input-event-codes.h>
#include <thread>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  void press(
      InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t mods = 0, bool repeat = false,
      std::uint32_t utf32 = 0, bool preedit = false
  ) {
    dispatcher.keyEvent(sym, utf32, mods, true, preedit, repeat);
  }

  void waitForDirectory(const filemanager::DirectorySession& session) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.loading() && std::chrono::steady_clock::now() < deadline) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    TEST_CHECK(!session.loading());
  }

  class TestDropdown final : public SelectPopupContext {
  public:
    void openSelectDropdown(const DropdownRequest&, DropdownCallbacks callbacks) override {
      ++opens;
      dismiss = std::move(callbacks.onDismiss);
    }

    void closeSelectDropdown() override {
      if (auto callback = std::exchange(dismiss, {}))
        callback();
    }

    [[nodiscard]] bool isSelectDropdownOpen() const override { return static_cast<bool>(dismiss); }

    int opens = 0;
    std::function<void()> dismiss;
  };

  class PendingClipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return std::nullopt; }
    void setClipboardText(std::string) override {}

    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override {
      TEST_CHECK(!done);
      done = std::move(callback);
    }

    void deliver(std::string text) {
      TEST_CHECK(done);
      std::exchange(done, {})(std::move(text));
    }

    std::function<void(std::optional<std::string>)> done;
  };

  class FileViewFixtureAdapter final : public VirtualGridAdapter {
  public:
    explicit FileViewFixtureAdapter(std::size_t count) : m_count(count) {}

    std::size_t itemCount() const override { return m_count; }
    std::unique_ptr<Node> createTile() override { return std::make_unique<Node>(); }
    void bindTile(Node&, std::size_t, bool, bool) override {}

  private:
    std::size_t m_count;
  };

  void testControls() {
    {
      int activations = 0;
      InputDispatcher dispatcher;
      Button button;
      button.setOnClick([&] { ++activations; });
      dispatcher.setSceneRoot(&button);
      dispatcher.setFocus(button.inputArea());
      press(dispatcher, XKB_KEY_space, KeyMod::Shift);
      press(dispatcher, XKB_KEY_space, 0, true);
      TEST_CHECK(activations == 0);
      press(dispatcher, XKB_KEY_space);
      TEST_CHECK(activations == 1);
    }
    {
      InputDispatcher dispatcher;
      Checkbox checkbox;
      dispatcher.setSceneRoot(&checkbox);
      dispatcher.setFocus(dispatcher.firstTabFocusUnder(&checkbox));
      TEST_CHECK(dispatcher.focusedArea() != nullptr);
      press(dispatcher, XKB_KEY_space, KeyMod::Ctrl);
      press(dispatcher, XKB_KEY_space, 0, true);
      TEST_CHECK(!checkbox.checked());
      press(dispatcher, XKB_KEY_space);
      TEST_CHECK(checkbox.checked());
    }
    {
      TestDropdown dropdown;
      InputDispatcher dispatcher;
      Select select;
      select.setOptions({"One", "Two"});
      select.setPopupContext(&dropdown);
      dispatcher.setSceneRoot(&select);
      dispatcher.setFocus(dispatcher.firstTabFocusUnder(&select));
      TEST_CHECK(dispatcher.focusedArea() != nullptr);
      press(dispatcher, XKB_KEY_Down, KeyMod::Alt, true);
      press(dispatcher, XKB_KEY_Down, KeyMod::Alt | KeyMod::Shift);
      TEST_CHECK(dropdown.opens == 0);
      press(dispatcher, XKB_KEY_Down, KeyMod::Alt);
      TEST_CHECK(select.open() && dropdown.opens == 1);
      dropdown.closeSelectDropdown();
      press(dispatcher, XKB_KEY_space);
      TEST_CHECK(select.open() && dropdown.opens == 2);
      dropdown.closeSelectDropdown();
    }
  }

  void testTextInput() {
    PendingClipboard clipboard;
    auto* previousClipboard = Input::textClipboard();
    struct RestoreClipboard {
      TextClipboard* previous;
      ~RestoreClipboard() { Input::setTextClipboard(previous); }
    } restore{previousClipboard};
    Input::setTextClipboard(&clipboard);
    InputDispatcher dispatcher;
    Input input;
    dispatcher.setSceneRoot(&input);
    dispatcher.setFocus(input.inputArea());
    int submits = 0;
    int escapes = 0;
    input.setOnSubmit([&](const std::string&) { ++submits; });
    input.setOnKeyEvent([&](std::uint32_t sym, std::uint32_t) {
      if (sym == XKB_KEY_Escape)
        ++escapes;
      return false;
    });

    press(dispatcher, XKB_KEY_x, KeyMod::Alt, false, 'x');
    press(dispatcher, XKB_KEY_x, KeyMod::Super, false, 'x');
    TEST_CHECK(input.value().empty());
    press(dispatcher, XKB_KEY_dead_acute, 0, false, U'´', true);
    press(dispatcher, XKB_KEY_Escape);
    TEST_CHECK(input.value().empty() && escapes == 0);
    press(dispatcher, XKB_KEY_Escape);
    TEST_CHECK(escapes == 1);
    press(dispatcher, XKB_KEY_Return, 0, true);
    TEST_CHECK(submits == 0);
    press(dispatcher, XKB_KEY_Return);
    TEST_CHECK(submits == 1);

    press(dispatcher, XKB_KEY_v, KeyMod::Ctrl, true);
    TEST_CHECK(!clipboard.done);
    press(dispatcher, XKB_KEY_v, KeyMod::Ctrl);
    TEST_CHECK(clipboard.done && input.value().empty());
    clipboard.deliver("a\r\nb\nc");
    TEST_CHECK(input.value().find_first_of("\r\n") == std::string::npos);
    TEST_CHECK(input.value().starts_with('a') && input.value().ends_with('c'));

    for (std::string invalid : {std::string("bad\0text", 8), std::string("bad\xFF")}) {
      const auto before = input.value();
      press(dispatcher, XKB_KEY_v, KeyMod::Ctrl);
      clipboard.deliver(std::move(invalid));
      TEST_CHECK(input.value() == before);
    }

    press(dispatcher, XKB_KEY_v, KeyMod::Ctrl);
    input.setValue("edited before paste arrived");
    clipboard.deliver("stale");
    TEST_CHECK(input.value() == "edited before paste arrived");
    press(dispatcher, XKB_KEY_v, KeyMod::Ctrl);
    dispatcher.setFocus(nullptr);
    clipboard.deliver("stale");
    TEST_CHECK(input.value() == "edited before paste arrived");
  }

} // namespace

class FileManagerTestAccess {
public:
  static void testView(FileManager& manager, FileViewFixtureAdapter& adapter) {
    waitForDirectory(manager.activeSession());
    auto& session = manager.activeSession();
    TEST_CHECK(session.entries().size() == 12);
    ScaledRenderer renderer(manager.m_renderContext, 1.0F);
    auto grid = std::make_unique<VirtualGridView>();
    auto* view = grid.get();
    grid->setAdapter(&adapter);
    grid->setSquareCells(false);
    grid->setCellHeight(24.0F);
    grid->setMinCellWidth(50.0F);
    grid->setFrameSize(240.0F, 68.0F);
    grid->setColumns(3);
    grid->layout(renderer);
    TEST_CHECK(grid->layoutColumnCount() == 3);
    manager.m_grid = view;
    manager.m_root = std::make_unique<Flex>();
    manager.m_root->setFrameSize(240.0F, 68.0F);
    manager.m_root->addChild(std::move(grid));
    manager.m_dispatcher.setSceneRoot(manager.m_root.get());
    manager.m_dispatcher.setFocus(view->focusArea());

    const auto key = [&](std::uint32_t sym, std::uint32_t mods = 0, bool repeat = false, std::uint32_t utf32 = 0) {
      manager.handleWindowKeyEvent({.sym = sym, .utf32 = utf32, .modifiers = mods, .pressed = true, .repeat = repeat});
    };
    const auto selected = [&](std::size_t index) {
      return manager.activeTab().selectedPaths.contains(session.entries()[index].absPath.string());
    };
    const auto selectedName = [&] {
      return manager.activeTab().selectedPath.value_or(std::filesystem::path{}).filename();
    };

    key(XKB_KEY_Home);
    TEST_CHECK(view->selectedIndex() == 0 && selected(0));
    key(XKB_KEY_Right);
    TEST_CHECK(view->selectedIndex() == 1 && selected(1) && !selected(0));
    key(XKB_KEY_Down, KeyMod::Ctrl, true);
    TEST_CHECK(view->selectedIndex() == 4 && selected(1) && !selected(4));
    key(XKB_KEY_space, KeyMod::Ctrl);
    TEST_CHECK(selected(1) && selected(4));
    key(XKB_KEY_space, KeyMod::Ctrl, true);
    TEST_CHECK(selected(4));
    key(XKB_KEY_Right, KeyMod::Shift);
    TEST_CHECK(!selected(1) && selected(4) && selected(5));
    key(XKB_KEY_Left, KeyMod::Ctrl | KeyMod::Shift);
    TEST_CHECK(view->selectedIndex() == 4 && selected(4) && selected(5));
    key(XKB_KEY_space);
    TEST_CHECK(manager.activeTab().selectedPaths.size() == 1 && selected(4));
    key(XKB_KEY_End);
    TEST_CHECK(view->selectedIndex() == 11 && selected(11));
    key(XKB_KEY_Home);
    const auto pageStride = view->pageItemStride();
    TEST_CHECK(pageStride >= 3 && pageStride < session.entries().size());
    key(XKB_KEY_Page_Down);
    TEST_CHECK(view->selectedIndex() == pageStride);
    key(XKB_KEY_Page_Up);
    TEST_CHECK(view->selectedIndex() == 0);

    view->setColumns(1);
    view->layout(renderer);
    TEST_CHECK(view->layoutColumnCount() == 1);
    key(XKB_KEY_Right);
    TEST_CHECK(view->selectedIndex() == 0);
    key(XKB_KEY_Down);
    TEST_CHECK(view->selectedIndex() == 1 && selected(1));
    TEST_CHECK(manager.m_fileViewFocusVisible);
    TEST_CHECK(manager.m_dispatcher.pointerButton(8, 8, BTN_LEFT, true, 1, 1, false));
    TEST_CHECK(manager.m_dispatcher.pointerButton(8, 8, BTN_LEFT, false, 2, 2, false));
    TEST_CHECK(manager.m_dispatcher.focusedArea() == view->focusArea());

    key(XKB_KEY_b, 0, false, 'b');
    TEST_CHECK(selectedName() == "Beta.txt");
    key(XKB_KEY_b, 0, true, 'b');
    TEST_CHECK(selectedName() == "Beta.txt");
    key(XKB_KEY_b, 0, false, 'b');
    TEST_CHECK(selectedName() == "Bravo.txt");
    key(XKB_KEY_e, 0, false, 'e');
    TEST_CHECK(selectedName() == "Beta.txt");
    manager.m_typeaheadLastTime -= std::chrono::seconds(1);
    key(XKB_KEY_g, 0, false, 'g');
    TEST_CHECK(selectedName() == "Gamma.txt");

    manager.m_dispatcher.setSceneRoot(nullptr);
    manager.m_grid = nullptr;
    manager.m_root.reset();
    const bool hidden = session.showHidden();
    key(XKB_KEY_h, KeyMod::Ctrl, true);
    TEST_CHECK(session.showHidden() == hidden);
    key(XKB_KEY_h, KeyMod::Ctrl);
    TEST_CHECK(session.showHidden() != hidden);
    waitForDirectory(session);
    TEST_CHECK(session.entries().size() == 13);
    const auto enumeration = session.enumerationGeneration();
    key(XKB_KEY_F5, 0, true);
    TEST_CHECK(session.enumerationGeneration() == enumeration);
    key(XKB_KEY_F5);
    TEST_CHECK(session.enumerationGeneration() > enumeration);
    waitForDirectory(session);

    const auto firstTab = manager.m_tabs.activeTabId();
    key(XKB_KEY_t, KeyMod::Ctrl, true);
    TEST_CHECK(manager.m_tabs.count() == 1);
    key(XKB_KEY_t, KeyMod::Ctrl);
    TEST_CHECK(manager.m_tabs.count() == 2);
    const auto secondTab = manager.m_tabs.activeTabId();
    TEST_CHECK(secondTab != firstTab);
    key(XKB_KEY_Page_Up, KeyMod::Ctrl);
    TEST_CHECK(manager.m_tabs.activeTabId() == firstTab);
    key(XKB_KEY_Page_Down, KeyMod::Ctrl);
    TEST_CHECK(manager.m_tabs.activeTabId() == secondTab);
    key(XKB_KEY_w, KeyMod::Ctrl, true);
    TEST_CHECK(manager.m_tabs.count() == 2);
    key(XKB_KEY_w, KeyMod::Ctrl);
    TEST_CHECK(manager.m_tabs.count() == 1 && manager.m_tabs.activeTabId() == firstTab);
  }

  template <typename Entry> static void testInlineRename(FileManager& manager, Entry& entry, bool directory) {
    int cancels = 0;
    int submits = 0;
    std::string submitted;
    const auto startingTabs = manager.m_tabs.count();
    auto* area = entry.beginInlineEdit(
        "/fixture/old.name", "old.name",
        [&](const std::string& name) {
          ++submits;
          submitted = name;
        },
        [&] { ++cancels; }, directory
    );
    manager.m_dispatcher.setSceneRoot(&entry);
    manager.m_dispatcher.setFocus(area);
    manager.m_inlineEditArea = area;
    const auto key = [&](std::uint32_t sym, std::uint32_t mods = 0, bool repeat = false, std::uint32_t utf32 = 0) {
      manager.handleWindowKeyEvent({.sym = sym, .utf32 = utf32, .modifiers = mods, .pressed = true, .repeat = repeat});
    };
    key(XKB_KEY_Tab);
    key(XKB_KEY_t, KeyMod::Ctrl, false, 't');
    key(XKB_KEY_h, KeyMod::Ctrl, false, 'h');
    TEST_CHECK(manager.m_dispatcher.focusedArea() == area && manager.m_tabs.count() == startingTabs);
    key(XKB_KEY_Escape, KeyMod::Alt);
    key(XKB_KEY_Escape, 0, true);
    key(XKB_KEY_Return, KeyMod::Shift);
    key(XKB_KEY_Return, 0, true);
    TEST_CHECK(cancels == 0 && submits == 0);
    key(XKB_KEY_n, 0, false, 'n');
    key(XKB_KEY_Return);
    TEST_CHECK(submits == 1 && submitted == (directory ? "n" : "n.name"));
    key(XKB_KEY_Escape);
    TEST_CHECK(cancels == 1);

    manager.m_inlineEditArea = nullptr;
    manager.m_dispatcher.setSceneRoot(nullptr);
    entry.endInlineEdit("/fixture/old.name");
  }

  static void testModalFallback(FileManager& manager) {
    auto& modal = manager.m_modalDialog;
    modal.m_title = "Keyboard focus";
    modal.m_choices = {{"OK", 1}};
    InputArea* choice = nullptr;
    modal.m_customContent.emplace();
    modal.m_customContent->initialFocus = []() -> InputArea* { return nullptr; };
    modal.m_customContent->bindChoiceButton = [&](int, Button* button) { choice = button->inputArea(); };
    modal.buildDialog(480.0F);
    TEST_CHECK(choice != nullptr && modal.m_dispatcher.focusedArea() == choice);
    modal.m_customContent.reset();
  }

  static void testProperties(FileManager& manager, const std::filesystem::path& file) {
    PropertiesWindow properties(
        manager.m_wayland, manager.m_renderContext, manager.m_configService, manager.m_operations, [](std::string) {}
    );
    const auto key = [&](std::uint32_t sym, std::uint32_t mods = 0, bool repeat = false) {
      properties.handleKeyForRoot({.sym = sym, .modifiers = mods, .pressed = true, .repeat = repeat});
    };

    properties.m_root = std::make_unique<Flex>();
    for (auto& tab : properties.m_tabButtons) {
      auto button = std::make_unique<Button>();
      button->setOnClick([] {});
      tab = button.get();
      properties.m_root->addChild(std::move(button));
    }
    properties.m_input.setSceneRoot(properties.m_root.get());
    properties.m_input.setFocus(properties.m_tabButtons[0]->inputArea());
    key(XKB_KEY_Right);
    TEST_CHECK(properties.m_tab == 1 && properties.m_input.focusedArea() == properties.m_tabButtons[1]->inputArea());
    key(XKB_KEY_Right, 0, true);
    TEST_CHECK(properties.m_tab == 1);
    key(XKB_KEY_Left);
    TEST_CHECK(properties.m_tab == 0 && properties.m_input.focusedArea() == properties.m_tabButtons[0]->inputArea());
    key(XKB_KEY_Page_Up, KeyMod::Ctrl);
    TEST_CHECK(properties.m_tab == 2);
    key(XKB_KEY_Tab, KeyMod::Ctrl, true);
    TEST_CHECK(properties.m_tab == 2);
    key(XKB_KEY_Page_Down, KeyMod::Ctrl);
    TEST_CHECK(properties.m_tab == 0);
    key(XKB_KEY_Tab, KeyMod::Ctrl);
    TEST_CHECK(properties.m_tab == 1);
    key(XKB_KEY_ISO_Left_Tab, KeyMod::Ctrl | KeyMod::Shift);
    TEST_CHECK(properties.m_tab == 0);

    properties.m_path = file;
    properties.m_nameDraft = file.filename().string();
    properties.m_itemAvailable = true;
    properties.m_general = {{.title = "Identity", .rows = {{"Name", properties.m_nameDraft}}}};
    key(XKB_KEY_F2, 0, true);
    TEST_CHECK(!properties.m_editingName);
    key(XKB_KEY_F2);
    TEST_CHECK(properties.m_editingName && properties.m_focusNameEditor);
    properties.m_editingName = false;

    properties.m_nameDraft = "staged";
    key(XKB_KEY_w, KeyMod::Ctrl, true);
    TEST_CHECK(!properties.m_discardRequested);
    key(XKB_KEY_w, KeyMod::Ctrl);
    TEST_CHECK(properties.m_discardRequested);
    key(XKB_KEY_Escape);
    TEST_CHECK(!properties.m_discardRequested);
    properties.m_input.setSceneRoot(nullptr);
  }

  static void run(const std::filesystem::path& root) {
    const auto browse = root / "browse";
    TEST_CHECK(std::filesystem::create_directory(browse));
    for (const auto* name :
         {"Alpha.txt", "Beta.txt", "Bravo.txt", "Delta.txt", "Echo.txt", "Foxtrot.txt", "Gamma.txt", "Hotel.txt",
          "India.txt", "Juliet.txt", "Kilo.txt", "Lima.txt", ".hidden"})
      TEST_CHECK(static_cast<bool>(std::ofstream(browse / name) << "fixture"));

    FileViewFixtureAdapter adapter(12);
    FileManager manager(browse.string());
    FileEntryRow row(1.0F);
    FileEntryTile tile(1.0F, nullptr);
    testControls();
    testTextInput();
    manager.enumerate(browse);
    testView(manager, adapter);
    testInlineRename(manager, row, false);
    testInlineRename(manager, tile, true);
    testModalFallback(manager);
    testProperties(manager, browse / "Alpha.txt");
  }
};

int main() {
  return nythera::test::runTest([] {
    nythera::test::ScopedTestEnv env({.prefix = "nythera-files-keyboard"});
    FileManagerTestAccess::run(env.work());
  });
}
