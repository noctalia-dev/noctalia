#include "core/timer_manager.h"
#include "render/animation/surface_haze.h"
#include "render/scene/rect_node.h"
#include "shell/settings/settings_content_common.h"
#include "test_check.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"

#include <chrono>
#include <thread>

int main() {
  using namespace std::chrono_literals;
  auto& service = SurfaceHaze::instance();
  int first = 0, second = 0, firstTicks = 0, secondTicks = 0, changes = 0;
  service.configure(true, true, false, {{"polkit", false}});
  service.attach(
      &first, "launcher",
      [&](float dt) {
        TEST_CHECK(dt > 0 && dt <= 0.05F);
        ++firstTicks;
      },
      [&] { ++changes; }
  );
  service.attach(&second, "notifications", [&](float) { ++secondTicks; }, [] {});
  service.configure(true, true, false, {{"polkit", false}});
  TEST_CHECK(changes == 0); // unrelated config reloads must not invalidate surfaces
  TEST_CHECK(service.style("launcher", 0, 1).enabled);
  TEST_CHECK(!service.style("polkit", 0, 2).enabled);
  TEST_CHECK(service.style("notifications", 0, 3).enabled);
  service.setReady(&first, true);
  service.setReady(&second, true);
  TEST_CHECK(service.timerActive());
  std::this_thread::sleep_for(36ms);
  TimerManager::instance().tick();
  TEST_CHECK(firstTicks == 1 && secondTicks == 1);
  TEST_CHECK(!service.timerActive()); // both wait for their compositor frame callback
  service.setReady(&first, true);
  service.setReady(&first, false); // hidden/occluded, or a frame already queued
  TEST_CHECK(!service.timerActive());
  std::this_thread::sleep_for(36ms);
  TimerManager::instance().tick();
  TEST_CHECK(firstTicks == 1 && secondTicks == 1);
  service.configure(true, false, true, {});
  service.setReady(&first, true);
  TEST_CHECK(!service.timerActive());
  TEST_CHECK(service.style("launcher", 4, 1).transparent);
  service.configure(false, true, false, {});
  TEST_CHECK(!service.style("launcher", 4, 1).enabled && !service.timerActive());
  service.configure(true, true, false, {{"launcher", false}});
  service.setReady(&first, true);
  service.setReady(&second, true);
  std::this_thread::sleep_for(36ms);
  TimerManager::instance().tick();
  TEST_CHECK(firstTicks == 1 && secondTicks == 2);
  service.detach(&first);
  service.detach(&second);
  TEST_CHECK(!service.timerActive());
  WaylandConnection wayland{WaylandConnection::ShellProtocols{}};
  LayerSurface bar(wayland, LayerSurfaceConfig{.nameSpace = "nythera-bar-main"});
  LayerSurface island(wayland, LayerSurfaceConfig{.nameSpace = "nythera-bar-dynamic-main"});
  LayerSurface toast(wayland, LayerSurfaceConfig{.nameSpace = "nythera-notification"});
  TEST_CHECK(bar.hazeProfile().empty() && island.hazeProfile().empty());
  TEST_CHECK(toast.hazeProfile() == "notifications");
  Box background;
  background.setPanelStyle();
  TEST_CHECK(background.style().surfaceBackground);
  auto style = background.style();
  style.radius = 7;
  background.setStyle(style);
  TEST_CHECK(background.style().surfaceBackground && background.style().radius.tl == 7);
  Box content;
  content.setFill(rgba(1, 0, 0, 1));
  TEST_CHECK(!content.style().surfaceBackground);

  // Settings decoration changes only under its own enabled profile, including
  // static/opaque mode. Disabling another surface must not change this policy.
  Config config;
  config.shell.panel.hazeSurfaces["launcher"] = false;
  for (auto mode : {PanelHazeMode::Static, PanelHazeMode::Animated}) {
    for (auto background : {PanelHazeBackground::Opaque, PanelHazeBackground::Transparent}) {
      config.shell.panel.hazeMode = mode;
      config.shell.panel.hazeBackground = background;
      config.shell.panel.hazeEnabled = true;
      config.shell.panel.hazeSurfaces.erase("settings");
      TEST_CHECK(settings::settingsHazeEnabled(config));
      config.shell.settingsWindowTranslucent = false;
      TEST_CHECK(resolveColorSpec(settings::settingsSidebarFill(config)).a == .4F);
      config.shell.panel.hazeSurfaces["settings"] = false;
      TEST_CHECK(!settings::settingsHazeEnabled(config));
      TEST_CHECK(resolveColorSpec(settings::settingsSidebarFill(config)).a == 1);
      config.shell.panel.hazeSurfaces["settings"] = true;
      config.shell.panel.hazeEnabled = false;
      TEST_CHECK(!settings::settingsHazeEnabled(config));
      TEST_CHECK(resolveColorSpec(settings::settingsSidebarFill(config)).a == 1);
      config.shell.settingsWindowTranslucent = true;
      TEST_CHECK(resolveColorSpec(settings::settingsSidebarFill(config)).a == 0);
    }
  }
  auto cardStyle = [](const Node& card) {
    for (const auto& child : card.children())
      if (child->type() == NodeType::Rect)
        return static_cast<const RectNode*>(child.get())->style();
    TEST_CHECK(false);
    return RoundedRectStyle{};
  };
  Flex normalCards, hazeCards, groupedCards;
  (void)settings::addSettingsCard(normalCards, "Normal", 1);
  auto* body = settings::addSettingsCard(hazeCards, "Haze", 1, true);
  std::unordered_set<std::string> expanded;
  (void)settings::addSettingsGroupCard({
      .parent = groupedCards, .group = "general", .title = "General",
      .expandedGroups = expanded, .hazeEnabled = true,
  });
  auto normal = cardStyle(*normalCards.children().front());
  const auto light = cardStyle(*hazeCards.children().front());
  TEST_CHECK(normal.fill.a == .4F && light.fill.a == .2F);
  TEST_CHECK(cardStyle(*groupedCards.children().front()) == light);
  normal.fill = light.fill;
  TEST_CHECK(normal == light); // borders, shape and native background tagging are unchanged
  TEST_CHECK(hazeCards.children().front()->opacity() == 1 && body->opacity() == 1);
  auto control = std::make_unique<Box>();
  control->setCardStyle();
  TEST_CHECK(control->style().fill.a == 1 && control->opacity() == 1);
  body->addChild(std::move(control));
}
