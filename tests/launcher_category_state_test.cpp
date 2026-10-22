#include "core/deferred_call.h"
#include "shell/launcher/launcher_panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/segmented.h"

#include <algorithm>
#include <string_view>
#include <vector>

class LauncherPanelTestAccess {
public:
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
