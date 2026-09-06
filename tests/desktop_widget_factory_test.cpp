#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "system/system_monitor_service.h"
#include "tests/test_check.h"
#include "ui/controls/label.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace {

  bool containsLabel(const Node& node) {
    if (dynamic_cast<const Label*>(&node) != nullptr) {
      return true;
    }
    for (const auto& child : node.children()) {
      if (containsLabel(*child)) {
        return true;
      }
    }
    return false;
  }

  std::unique_ptr<DesktopWidget> makeSysmon(const DesktopWidgetFactory& factory, bool showLabel) {
    std::unordered_map<std::string, WidgetSettingValue> settings{
        {"network_speed_compact", true},
        {"network_speed_decimal_places", std::int64_t{2}},
        {"show_label", showLabel},
        {"stat", std::string("net_rx")},
    };
    auto widget = factory.create("sysmon", settings);
    TEST_CHECK(widget != nullptr);
    widget->create();
    TEST_CHECK(widget->root() != nullptr);
    return widget;
  }

} // namespace

int main() {
  SystemConfig::MonitorConfig monitorConfig;
  monitorConfig.enabled = false;
  SystemMonitorService monitor(monitorConfig);
  DesktopWidgetFactory factory(DesktopWidgetRuntimeServices{.sysmon = &monitor});

  const auto hiddenLabel = makeSysmon(factory, false);
  TEST_CHECK(!containsLabel(*hiddenLabel->root()));

  const auto visibleLabel = makeSysmon(factory, true);
  TEST_CHECK(containsLabel(*visibleLabel->root()));
  return 0;
}
