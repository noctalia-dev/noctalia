#include "config/config_types.h"
#include "render/scene/input_area.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_gesture.h"
#include "shell/settings/settings_control_factory.h"
#include "tests/test_check.h"
#include "ui/controls/input.h"

#include <array>
#include <linux/input-event-codes.h>
#include <string>
#include <wayland-client-protocol.h>

namespace {
  Input* findInput(Node& node) {
    if (auto* input = dynamic_cast<Input*>(&node)) {
      return input;
    }
    for (const auto& child : node.children()) {
      if (auto* input = findInput(*child)) {
        return input;
      }
    }
    return nullptr;
  }

  using namespace noctalia::bar;

  void testGestureKeys() {
    TEST_CHECK(allGestures().size() == kGestureCount);

    // Every gesture round-trips through its config key, and every key is distinct.
    for (const auto gesture : allGestures()) {
      const auto key = gestureConfigKey(gesture);
      TEST_CHECK(!key.empty());
      const auto parsed = parseGestureKey(key);
      TEST_CHECK(parsed.has_value());
      TEST_CHECK(*parsed == gesture);
      TEST_CHECK(!gestureLabelKey(gesture).empty());
    }

    // Modifier chords are not part of the vocabulary: the bar never holds keyboard focus.
    TEST_CHECK(!parseGestureKey("ctrl+left").has_value());
    TEST_CHECK(!parseGestureKey("Left").has_value());
    TEST_CHECK(!parseGestureKey("").has_value());
    TEST_CHECK(!parseGestureKey("scroll").has_value());
  }

  void testButtonAndScrollMapping() {
    TEST_CHECK(gestureForButton(BTN_LEFT) == Gesture::Left);
    TEST_CHECK(gestureForButton(BTN_RIGHT) == Gesture::Right);
    TEST_CHECK(gestureForButton(BTN_MIDDLE) == Gesture::Middle);
    // Mice report thumb buttons either way.
    TEST_CHECK(gestureForButton(BTN_SIDE) == Gesture::Back);
    TEST_CHECK(gestureForButton(BTN_BACK) == Gesture::Back);
    TEST_CHECK(gestureForButton(BTN_EXTRA) == Gesture::Forward);
    TEST_CHECK(gestureForButton(BTN_FORWARD) == Gesture::Forward);
    TEST_CHECK(!gestureForButton(BTN_TASK).has_value());

    TEST_CHECK(buttonsForGesture(Gesture::Back).size() == 2);
    TEST_CHECK(buttonsForGesture(Gesture::ScrollUp).empty());

    // Wayland reports up/left as a negative delta.
    TEST_CHECK(gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, -1.0F) == Gesture::ScrollUp);
    TEST_CHECK(gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, 1.0F) == Gesture::ScrollDown);
    TEST_CHECK(gestureForScroll(WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1.0F) == Gesture::ScrollLeft);
    TEST_CHECK(gestureForScroll(WL_POINTER_AXIS_HORIZONTAL_SCROLL, 1.0F) == Gesture::ScrollRight);
    // No whole detent accumulated yet.
    TEST_CHECK(!gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0F).has_value());
  }

  void testScrollRepeatModes() {
    TEST_CHECK(parseScrollRepeatMode("auto") == ScrollRepeatMode::Auto);
    TEST_CHECK(parseScrollRepeatMode("gesture") == ScrollRepeatMode::Gesture);
    TEST_CHECK(parseScrollRepeatMode("steps") == ScrollRepeatMode::Steps);
    TEST_CHECK(!parseScrollRepeatMode("cycle").has_value());
    TEST_CHECK(!parseScrollRepeatMode("").has_value());

    TEST_CHECK(scrollRepeatsEveryStep(ScrollRepeatMode::Auto, false));
    TEST_CHECK(!scrollRepeatsEveryStep(ScrollRepeatMode::Auto, true));
    TEST_CHECK(!scrollRepeatsEveryStep(ScrollRepeatMode::Gesture, false));
    TEST_CHECK(!scrollRepeatsEveryStep(ScrollRepeatMode::Gesture, true));
    TEST_CHECK(scrollRepeatsEveryStep(ScrollRepeatMode::Steps, false));
    TEST_CHECK(scrollRepeatsEveryStep(ScrollRepeatMode::Steps, true));
  }

  void testActionGrammar() {
    const auto ipc = parseWidgetAction("media toggle");
    TEST_CHECK(ipc.has_value());
    TEST_CHECK(ipc->kind == WidgetAction::Kind::Ipc);
    TEST_CHECK(ipc->verb == "media");
    TEST_CHECK(ipc->args == "toggle");
    TEST_CHECK(ipc->commandLine() == "media toggle");
    TEST_CHECK(ipc->isBound());

    const auto bare = parseWidgetAction("power-cycle");
    TEST_CHECK(bare.has_value());
    TEST_CHECK(bare->verb == "power-cycle");
    TEST_CHECK(bare->args.empty());
    TEST_CHECK(bare->commandLine() == "power-cycle");

    const auto multiArg = parseWidgetAction("  panel-toggle control-center media  ");
    TEST_CHECK(multiArg.has_value());
    TEST_CHECK(multiArg->verb == "panel-toggle");
    TEST_CHECK(multiArg->args == "control-center media");

    const auto exec = parseWidgetAction("exec notify-send hello world");
    TEST_CHECK(exec.has_value());
    TEST_CHECK(exec->kind == WidgetAction::Kind::Exec);
    TEST_CHECK(exec->args == "notify-send hello world");

    const auto none = parseWidgetAction("none");
    TEST_CHECK(none.has_value());
    TEST_CHECK(none->kind == WidgetAction::Kind::None);
    TEST_CHECK(!none->isBound());

    // An unknown verb never falls through to a shell command, and empty is an error rather than a
    // silent synonym for "none".
    TEST_CHECK(!parseWidgetAction("").has_value());
    TEST_CHECK(!parseWidgetAction("   ").has_value());
    TEST_CHECK(!parseWidgetAction("exec").has_value());
    TEST_CHECK(!parseWidgetAction("exec   ").has_value());
    TEST_CHECK(!parseWidgetAction("none please").has_value());
  }

  void testPanelVerbs() {
    TEST_CHECK(isAnchoredPanelVerb("panel-toggle"));
    TEST_CHECK(isAnchoredPanelVerb("panel-open"));
    TEST_CHECK(!isAnchoredPanelVerb("panel-close"));
    TEST_CHECK(!isAnchoredPanelVerb("media"));

    const auto withContext = parsePanelVerbArgs("control-center media");
    TEST_CHECK(withContext.panelId == "control-center");
    TEST_CHECK(withContext.panelContext == "media");

    const auto bare = parsePanelVerbArgs("session");
    TEST_CHECK(bare.panelId == "session");
    TEST_CHECK(bare.panelContext.empty());
  }

  WidgetConfig makeConfig(std::unordered_map<std::string, std::string> actions) {
    WidgetConfig config;
    config.tables["actions"] = std::move(actions);
    return config;
  }

  void testLayeredResolution() {
    constexpr std::array kBuiltin{GestureBinding{Gesture::Middle, "settings-open-widget"}};
    constexpr std::array kWidgetDefaults{
        GestureBinding{Gesture::Left, "panel-toggle control-center media"},
        GestureBinding{Gesture::Right, "media toggle"},
    };

    // Layers 1 and 2 alone.
    {
      WidgetActionBindings bindings;
      bindings.resolve(WidgetActionBindings::Inputs{.builtinDefaults = kBuiltin, .widgetDefaults = kWidgetDefaults});
      TEST_CHECK(bindings.find(Gesture::Middle)->verb == "settings-open-widget");
      TEST_CHECK(bindings.find(Gesture::Left)->verb == "panel-toggle");
      TEST_CHECK(bindings.find(Gesture::Right)->args == "toggle");
      TEST_CHECK(bindings.find(Gesture::Forward) == nullptr);
      TEST_CHECK(!bindings.empty());
      TEST_CHECK(bindings.boundGestures().contains(Gesture::Left));
      TEST_CHECK(!bindings.boundGestures().contains(Gesture::Forward));
    }

    // Layer 2 can unbind a built-in too. This is how a plugin [[widget]] whose script implements
    // onMiddleClick declares that middle belongs to it: nothing bound means the widget's own input
    // area keeps the button.
    {
      constexpr std::array kFreesMiddle{GestureBinding{Gesture::Middle, "none"}};
      WidgetActionBindings bindings;
      bindings.resolve(WidgetActionBindings::Inputs{.builtinDefaults = kBuiltin, .widgetDefaults = kFreesMiddle});
      TEST_CHECK(bindings.find(Gesture::Middle) == nullptr);
      TEST_CHECK(!bindings.boundGestures().contains(Gesture::Middle));
      TEST_CHECK(bindings.empty());
    }

    // Layer 4 overrides a widget default; "none" unbinds an inherited one.
    {
      const WidgetConfig widget = makeConfig({{"right", "media next"}, {"left", "none"}});
      WidgetActionBindings bindings;
      bindings.resolve(
          WidgetActionBindings::Inputs{
              .builtinDefaults = kBuiltin,
              .widgetDefaults = kWidgetDefaults,
              .widgetActions = findActionTable(&widget),
              .widgetContext = "widget.media",
          }
      );
      TEST_CHECK(bindings.find(Gesture::Right)->args == "next");
      TEST_CHECK(bindings.find(Gesture::Left) == nullptr);
      TEST_CHECK(bindings.find(Gesture::Middle)->verb == "settings-open-widget");
    }

    // Layer 3 beats defaults, layer 4 beats layer 3.
    {
      const WidgetConfig bar = makeConfig({{"middle", "none"}, {"right", "volume-mute"}});
      const WidgetConfig widget = makeConfig({{"right", "media toggle"}});
      WidgetActionBindings bindings;
      bindings.resolve(
          WidgetActionBindings::Inputs{
              .builtinDefaults = kBuiltin,
              .widgetDefaults = kWidgetDefaults,
              .barActions = findActionTable(&bar),
              .widgetActions = findActionTable(&widget),
              .widgetContext = "widget.media",
              .barContext = "bar.default",
          }
      );
      TEST_CHECK(bindings.find(Gesture::Middle) == nullptr);
      TEST_CHECK(bindings.find(Gesture::Right)->verb == "media");
    }

    // Reserved gestures never bind, whether they came from a default or from config.
    {
      const WidgetConfig widget = makeConfig({{"left", "media toggle"}});
      WidgetActionBindings bindings;
      bindings.resolve(
          WidgetActionBindings::Inputs{
              .builtinDefaults = kBuiltin,
              .widgetDefaults = kWidgetDefaults,
              .widgetActions = findActionTable(&widget),
              .reserved = GestureMask{Gesture::Left},
              .widgetContext = "widget.workspaces",
              .widgetName = "workspaces",
          }
      );
      TEST_CHECK(bindings.find(Gesture::Left) == nullptr);
      TEST_CHECK(bindings.find(Gesture::Middle) != nullptr);
    }

    // An unrecognised verb still parses and binds: whether a command exists is settled at
    // dispatch time against the live registry, since bars are built before IPC registration.
    {
      const WidgetConfig widget = makeConfig({{"right", "medai toggle"}});
      WidgetActionBindings bindings;
      bindings.resolve(
          WidgetActionBindings::Inputs{
              .builtinDefaults = kBuiltin,
              .widgetDefaults = kWidgetDefaults,
              .widgetActions = findActionTable(&widget),
              .widgetContext = "widget.media",
          }
      );
      const auto* bound = bindings.find(Gesture::Right);
      TEST_CHECK(bound != nullptr);
      TEST_CHECK(bound->verb == "medai");
      TEST_CHECK(bound->commandLine() == "medai toggle");
    }

    // Unknown gesture keys are dropped without disturbing valid siblings.
    {
      const WidgetConfig widget = makeConfig({{"ctrl+left", "media toggle"}, {"forward", "media next"}});
      WidgetActionBindings bindings;
      bindings.resolve(
          WidgetActionBindings::Inputs{
              .widgetActions = findActionTable(&widget),
              .widgetContext = "widget.media",
          }
      );
      TEST_CHECK(bindings.find(Gesture::Forward)->args == "next");
      TEST_CHECK(bindings.boundGestures().contains(Gesture::Forward));
    }

    // No actions table at all.
    {
      const WidgetConfig empty;
      TEST_CHECK(findActionTable(&empty) == nullptr);
      TEST_CHECK(findActionTable(nullptr) == nullptr);
      WidgetActionBindings bindings;
      bindings.resolve(WidgetActionBindings::Inputs{});
      TEST_CHECK(bindings.empty());
    }
  }

  void testInheritedActionArgumentsRemainEditable() {
    Config config;
    std::string editingWidgetName;
    std::string editingCapsuleGroupId;
    std::vector<std::string> selectedLaneWidgets;
    std::string pendingDeleteWidgetName;
    std::string pendingDeleteWidgetSettingPath;
    std::string renamingWidgetName;
    std::string pendingGestureKey;
    std::string pendingGestureVerb;
    std::string actionsExpandedFor;
    std::string storedAction;
    std::vector<std::string> storedPath;

    settings::SettingsControlFactory factory(
        settings::SettingsContentContext{
            .config = config,
            .editingWidgetName = editingWidgetName,
            .editingCapsuleGroupId = editingCapsuleGroupId,
            .selectedLaneWidgets = selectedLaneWidgets,
            .pendingDeleteWidgetName = pendingDeleteWidgetName,
            .pendingDeleteWidgetSettingPath = pendingDeleteWidgetSettingPath,
            .renamingWidgetName = renamingWidgetName,
            .pendingGestureKey = pendingGestureKey,
            .pendingGestureVerb = pendingGestureVerb,
            .actionsExpandedFor = actionsExpandedFor,
            .actionCatalog = {settings::GestureActionOption{
                .option = settings::SelectOption{.value = "volume-up", .label = "volume-up"},
                .argsSpec = "[step]",
            }},
            .setOverride =
                [&storedAction, &storedPath](std::vector<std::string> path, ConfigOverrideValue value) {
                  storedPath = std::move(path);
                  storedAction = std::get<std::string>(std::move(value));
                },
            .clearOverride = [](std::vector<std::string>) {},
        }
    );

    auto inherited = factory.makeGestureActionRow(
        settings::GestureActionSetting{
            .gestureKey = "scroll_up",
            .defaultAction = "volume-up",
        },
        "Scroll up", {"widget", "volume", "actions", "scroll_up"}
    );
    auto* inheritedStep = findInput(*inherited);
    TEST_CHECK(inheritedStep != nullptr);
    TEST_CHECK(inheritedStep->value().empty());
    inheritedStep->setValue("12%");
    inheritedStep->inputArea()->dispatchFocusLoss();
    TEST_CHECK(storedAction == "volume-up 12%");
    TEST_CHECK(storedPath == std::vector<std::string>({"widget", "volume", "actions", "scroll_up"}));

    auto configured = factory.makeGestureActionRow(
        settings::GestureActionSetting{
            .gestureKey = "scroll_up",
            .configured = "volume-up 12%",
            .defaultAction = "volume-up",
        },
        "Scroll up", {"widget", "volume", "actions", "scroll_up"}
    );
    const auto* configuredStep = findInput(*configured);
    TEST_CHECK(configuredStep != nullptr);
    TEST_CHECK(configuredStep->value() == "12%");
  }

} // namespace

int main() {
  testGestureKeys();
  testButtonAndScrollMapping();
  testScrollRepeatModes();
  testActionGrammar();
  testPanelVerbs();
  testLayeredResolution();
  testInheritedActionArgumentsRemainEditable();
  return 0;
}
