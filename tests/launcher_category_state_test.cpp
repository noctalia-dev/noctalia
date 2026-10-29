#include "core/deferred_call.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/segmented.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
  class LayoutProvider final : public LauncherProvider {
  public:
    explicit LayoutProvider(bool grid) : m_grid(grid) {}
    std::string_view defaultPrefix() const override { return m_grid ? "grid" : "list"; }
    std::string_view id() const override { return m_grid ? "Grid" : "List"; }
    bool supportsGrid() const override { return m_grid; }
    bool defaultIncludeInGlobalSearch() const override { return true; }
    std::vector<LauncherResult> query(std::string_view text) const override {
      if (text == "empty") {
        return {};
      }
      return {{.id = std::string(id()), .title = "Result", .presentation = text == "detail" ? "detail" : ""}};
    }
    bool activate(const LauncherResult&) override { return false; }

  private:
    bool m_grid;
  };
} // namespace

class LauncherPanelTestAccess {
public:
  static void checkGridPolicy() {
    LauncherPanel panel(nullptr, nullptr, nullptr);
    panel.addProvider(std::make_unique<LayoutProvider>(true));
    panel.addProvider(std::make_unique<LayoutProvider>(false));

    // Prefix routing supplies the policy even when no results are available.
    panel.onInputChanged("/grid");
    TEST_CHECK(panel.supportsResultGrid());
    panel.onInputChanged("/grid empty");
    TEST_CHECK(panel.m_results.empty());
    TEST_CHECK(panel.supportsResultGrid());
    panel.onInputChanged("/list empty");
    TEST_CHECK(!panel.supportsResultGrid());
    panel.onInputChanged("/grid detail");
    TEST_CHECK(!panel.supportsResultGrid());

    // Mixed global results must retain the list-only provider's text.
    panel.onInputChanged("test");
    TEST_CHECK(panel.m_results.size() == 2);
    TEST_CHECK(!panel.supportsResultGrid());
    std::erase_if(panel.m_results, [](const auto& result) { return result.providerId == "List"; });
    TEST_CHECK(panel.supportsResultGrid());

    panel.onInputChanged("/");
    TEST_CHECK(panel.supportsResultGrid());
    panel.m_launcherShowIcons = false;
    TEST_CHECK(!panel.supportsResultGrid());
    panel.m_launcherShowIcons = true;

    // A selected provider and an explicit scope use the same policy as its prefix.
    panel.m_launcherProviderViewId = "Grid";
    panel.onInputChanged("");
    TEST_CHECK(panel.supportsResultGrid());
    panel.m_launcherProviderViewId.clear();
    panel.setScopedProvider("List");
    panel.onInputChanged("/grid");
    TEST_CHECK(!panel.supportsResultGrid());
    panel.setScopedProvider("Grid");
    panel.onInputChanged("");
    TEST_CHECK(panel.supportsResultGrid());
  }

  static void initialize(LauncherPanel& panel, Segmented& navigation, Segmented& categories) {
    panel.m_browserMode = true;
    panel.m_launcherNavigation = &navigation;
    panel.m_categoryFilter = &categories;
    panel.m_allResults = {
        {.id = "browser", .category = "Internet"},
        {.id = "editor", .category = "Development"},
        {.id = "terminal", .category = "Development"},
    };
    panel.updateCategoryFilterModel({{"Internet", "world"}, {"Development", "code"}});
    panel.rebuildCategoryFilter();
  }

  static std::size_t slot(const LauncherPanel& panel, LauncherPanel::ActiveCategoryType type) {
    const auto it = std::ranges::find(panel.m_categoryFilterSlots, type, &LauncherPanel::CategoryFilterSlot::type);
    TEST_CHECK(it != panel.m_categoryFilterSlots.end());
    return static_cast<std::size_t>(it - panel.m_categoryFilterSlots.begin());
  }

  static std::size_t categoriesSlot(const LauncherPanel& panel) { return slot(panel, LauncherPanel::Category); }
  static std::size_t allSlot(const LauncherPanel& panel) { return slot(panel, LauncherPanel::All); }

  static void expectResults(const LauncherPanel& panel, const std::vector<std::string_view>& ids) {
    std::vector<std::string_view> actual;
    actual.reserve(panel.m_results.size());
    for (const auto& result : panel.m_results) {
      actual.push_back(result.id);
    }
    TEST_CHECK(actual == ids);
  }

  static void rebuildCategories(LauncherPanel& panel) { panel.rebuildCategoryFilter(); }
};

namespace {
  void selectTab(Segmented& navigation, std::size_t index) {
    navigation.setSelectedIndex(index);
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }
} // namespace

int main() {
  PanelManager manager;
  LauncherPanelTestAccess::checkGridPolicy();
  Segmented navigation;
  Segmented categories;
  LauncherPanel panel(nullptr, nullptr, nullptr);
  LauncherPanelTestAccess::initialize(panel, navigation, categories);
  selectTab(navigation, LauncherPanelTestAccess::categoriesSlot(panel));
  categories.setSelectedIndex(1);
  LauncherPanelTestAccess::expectResults(panel, {"editor", "terminal"});

  selectTab(navigation, LauncherPanelTestAccess::allSlot(panel));
  LauncherPanelTestAccess::expectResults(panel, {"browser", "editor", "terminal"});
  // Rebuilding the category row while another view is active must not switch back to Categories.
  LauncherPanelTestAccess::rebuildCategories(panel);
  TEST_CHECK(navigation.selectedIndex() == LauncherPanelTestAccess::allSlot(panel));

  // Returning to Categories restores the category picked before leaving.
  selectTab(navigation, LauncherPanelTestAccess::categoriesSlot(panel));
  TEST_CHECK(categories.selectedIndex() == 1);
  LauncherPanelTestAccess::expectResults(panel, {"editor", "terminal"});

  return 0;
}
