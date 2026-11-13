#include "shell/desktop/desktop_widget_layout.h"
#include "shell/lockscreen/lockscreen_login_box.h"
#include "tests/test_check.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

  WaylandOutput makeOutput(std::string connector, std::string model, std::string serial, int width, int height) {
    static char outputToken;
    WaylandOutput output;
    output.connectorName = std::move(connector);
    output.description = "Acme " + model;
    output.make = "Acme";
    output.model = std::move(model);
    output.serialNumber = std::move(serial);
    // Selection only reads output metadata; the proxy is never dereferenced.
    output.output = reinterpret_cast<wl_output*>(&outputToken);
    output.done = true;
    output.logicalWidth = width;
    output.logicalHeight = height;
    return output;
  }

  DesktopWidgetState makeWidget(std::string id, std::string selector, float width, float height) {
    DesktopWidgetState widget;
    widget.id = std::move(id);
    widget.outputName = std::move(selector);
    widget.cx = width * 0.25F;
    widget.cy = height * 0.75F;
    widget.placementWidth = width;
    widget.placementHeight = height;
    return widget;
  }

  void checkSelection() {
    std::vector<WaylandOutput> outputs{
        makeOutput("DP-3", "Landscape", "LANDSCAPE123", 2560, 1440),
        makeOutput("DP-4", "Portrait", "PORTRAIT456", 1296, 2304),
    };
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-3").output == outputs.data());
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme Landscape").output == outputs.data());
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == &outputs[1]);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "PORTRAIT456").output == &outputs[1]);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "PORTRAIT").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "portrait").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").output == nullptr);
    TEST_CHECK(!desktop_widgets::selectOutput(outputs, "missing").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "missing").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "").output == outputs.data());

    // An exact connector assignment wins even if another description contains it.
    outputs[0].description = "Acme DP-4";
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-4").output == &outputs[1]);
    std::ranges::reverse(outputs);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-4").output == outputs.data());

    outputs[0].done = false;
    TEST_CHECK(!desktop_widgets::selectOutput(outputs, "Acme").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").output == &outputs[1]);
    outputs[0].done = true;
    outputs[0].output = nullptr;
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == nullptr);
    outputs[0] = makeOutput("DP-4", "Portrait", "PORTRAIT456", 0, 0);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == nullptr);
  }

  void checkPlacementAcrossDocking() {
    std::vector<WaylandOutput> outputs{
        makeOutput("DP-3", "Landscape", "LANDSCAPE123", 2560, 1440),
        makeOutput("DP-4", "Portrait", "PORTRAIT456", 1296, 2304),
    };
    std::vector<DesktopWidgetState> widgets{
        makeWidget("landscape-clock", "Landscape", 2560.0F, 1440.0F),
        makeWidget("portrait-clock", "PORTRAIT456", 1296.0F, 2304.0F),
    };
    const auto original = widgets;
    const desktop_widgets::PlacementMapper mapper;
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));

    // Connectors can be reused by the other physical monitor after docking.
    std::swap(outputs[0].connectorName, outputs[1].connectorName);
    std::ranges::reverse(outputs);
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == original);

    outputs.clear();
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == original);

    outputs.push_back(makeOutput("DP-11", "Landscape", "LANDSCAPE123", 1280, 720));
    TEST_CHECK(mapper.remapForOutputChange(outputs, widgets));
    TEST_CHECK(widgets[0].cx == original[0].cx * 0.5F);
    TEST_CHECK(widgets[0].cy == original[0].cy * 0.5F);
    TEST_CHECK(widgets[0].outputName == "Landscape");
    TEST_CHECK(widgets[1] == original[1]);
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));

    // Ambiguity must never choose a monitor and persist its geometry.
    outputs.push_back(makeOutput("DP-12", "Landscape", "ANOTHER789", 3840, 2160));
    const auto beforeAmbiguity = widgets;
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == beforeAmbiguity);
  }

  void checkLoginBoxes() {
    std::vector<WaylandOutput> outputs{
        makeOutput("DP-3", "Landscape", "LANDSCAPE123", 2560, 1440),
        makeOutput("DP-4", "Portrait", "PORTRAIT456", 1296, 2304),
    };
    std::vector<DesktopWidgetState> widgets{
        makeWidget("lockscreen-login-box@DP-3", "LANDSCAPE123", 2560.0F, 1440.0F),
        makeWidget("lockscreen-login-box@DP-4", "Portrait", 1296.0F, 2304.0F),
    };
    for (auto& widget : widgets) {
      widget.type = "login_box";
    }
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 2);
    const auto original = widgets;
    std::swap(outputs[0].connectorName, outputs[1].connectorName);
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets == original);
    TEST_CHECK(lockscreen_login_box::findForOutput(widgets, outputs, "DP-4") == widgets.data());
    TEST_CHECK(lockscreen_login_box::findForOutput(widgets, outputs, "DP-3") == &widgets[1]);

    outputs.erase(outputs.begin());
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets == original);
    widgets[1].enabled = false;
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets[1].enabled);

    // A disabled matching box still covers its output; it must not gain a duplicate.
    outputs.insert(outputs.begin(), makeOutput("DP-11", "Landscape", "LANDSCAPE123", 2560, 1440));
    widgets[0].enabled = false;
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 2);
    TEST_CHECK(!widgets[0].enabled);
    TEST_CHECK(lockscreen_login_box::findForOutput(widgets, outputs, "DP-11") == widgets.data());

    widgets[0].outputName = "Acme";
    const auto ambiguous = widgets[0];
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 3);
    const auto retained = std::ranges::find(widgets, ambiguous.id, &DesktopWidgetState::id);
    TEST_CHECK(retained != widgets.end());
    TEST_CHECK(*retained == ambiguous);
    const auto* generated = lockscreen_login_box::findForOutput(widgets, outputs, "DP-11");
    TEST_CHECK(generated != nullptr);
    TEST_CHECK(generated->outputName == "DP-11");
    TEST_CHECK(generated->enabled);

    widgets = original;
    widgets[1].enabled = false;
    auto exact = widgets[0];
    exact.id = "exact-connector-login";
    exact.outputName = "DP-11";
    exact.enabled = false;
    widgets.insert(widgets.begin(), exact);
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 3);
    const auto* chosen = lockscreen_login_box::findForOutput(widgets, outputs, "DP-11");
    TEST_CHECK(chosen == widgets.data());
    // A shadowed enabled box cannot satisfy the connected-login visibility requirement.
    TEST_CHECK(chosen->enabled || widgets[2].enabled);

    // Identity metadata can arrive after a default box has already been created.
    widgets = original;
    outputs[0].serialNumber.clear();
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 3);
    outputs[0].serialNumber = "LANDSCAPE123";
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 3);
    TEST_CHECK(lockscreen_login_box::findForOutput(widgets, outputs, "DP-11") == widgets.data());
    TEST_CHECK(widgets[0] == original[0]);

    widgets = original;
    outputs[1].connectorName = "DP-12";
    outputs.push_back(makeOutput("DP-3", "NewMonitor", "NEW789", 1920, 1080));
    lockscreen_login_box::ensureWidgets(widgets, outputs);
    TEST_CHECK(widgets.size() == 3);
    const auto* newBox = lockscreen_login_box::findForOutput(widgets, outputs, "DP-3");
    TEST_CHECK(newBox != nullptr);
    TEST_CHECK(newBox->id != original[0].id);
    const auto oldBox = std::ranges::find(widgets, original[0].id, &DesktopWidgetState::id);
    TEST_CHECK(oldBox != widgets.end());
    TEST_CHECK(oldBox->outputName == original[0].outputName);
    for (std::size_t i = 0; i < widgets.size(); ++i) {
      for (std::size_t j = i + 1; j < widgets.size(); ++j) {
        TEST_CHECK(widgets[i].id != widgets[j].id);
      }
    }
  }

} // namespace

int main() {
  checkSelection();
  checkPlacementAcrossDocking();
  checkLoginBoxes();
  return 0;
}
