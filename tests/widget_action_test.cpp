#include "config/config_types.h"
#include "render/scene/input_area.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_gesture.h"
#include "shell/settings/settings_control_factory.h"
#include "ui/controls/input.h"

#include <array>
#include <cassert>
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
    assert(allGestures().size() == kGestureCount);

    // Every gesture round-trips through its config key, and every key is distinct.
    for (const auto gesture : allGestures()) {
      const auto key = gestureConfigKey(gesture);
      assert(!key.empty());
      const auto parsed = parseGestureKey(key);
      assert(parsed.has_value());
      assert(*parsed == gesture);
      assert(!gestureLabelKey(gesture).empty());
    }

    // Modifier chords are not part of the vocabulary: the bar never holds keyboard focus.
    assert(!parseGestureKey("ctrl+left").has_value());
    assert(!parseGestureKey("Left").has_value());
    assert(!parseGestureKey("").has_value());
    assert(!parseGestureKey("scroll").has_value());
  }

  void testButtonAndScrollMapping() {
    assert(gestureForButton(BTN_LEFT) == Gesture::Left);
    assert(gestureForButton(BTN_RIGHT) == Gesture::Right);
    assert(gestureForButton(BTN_MIDDLE) == Gesture::Middle);
    // Mice report thumb buttons either way.
    assert(gestureForButton(BTN_SIDE) == Gesture::Back);
    assert(gestureForButton(BTN_BACK) == Gesture::Back);
    assert(gestureForButton(BTN_EXTRA) == Gesture::Forward);
    assert(gestureForButton(BTN_FORWARD) == Gesture::Forward);
    assert(!gestureForButton(BTN_TASK).has_value());

    assert(buttonsForGesture(Gesture::Back).size() == 2);
    assert(buttonsForGesture(Gesture::ScrollUp).empty());

    // Wayland reports up/left as a negative delta.
    assert(gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, -1.0f) == Gesture::ScrollUp);
    assert(gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, 1.0f) == Gesture::ScrollDown);
    assert(gestureForScroll(WL_POINTER_AXIS_HORIZONTAL_SCROLL, -1.0f) == Gesture::ScrollLeft);
    assert(gestureForScroll(WL_POINTER_AXIS_HORIZONTAL_SCROLL, 1.0f) == Gesture::ScrollRight);
    // No whole detent accumulated yet.
    assert(!gestureForScroll(WL_POINTER_AXIS_VERTICAL_SCROLL, 0.0f).has_value());
  }

  void testScrollRepeatModes() {
    assert(parseScrollRepeatMode("auto") == ScrollRepeatMode::Auto);
    assert(parseScrollRepeatMode("gesture") == ScrollRepeatMode::Gesture);
    assert(parseScrollRepeatMode("steps") == ScrollRepeatMode::Steps);
    assert(!parseScrollRepeatMode("cycle").has_value());
    assert(!parseScrollRepeatMode("").has_value());

    assert(scrollRepeatsEveryStep(ScrollRepeatMode::Auto, false));
    assert(!scrollRepeatsEveryStep(ScrollRepeatMode::Auto, true));
    assert(!scrollRepeatsEveryStep(ScrollRepeatMode::Gesture, false));
    assert(!scrollRepeatsEveryStep(ScrollRepeatMode::Gesture, true));
    assert(scrollRepeatsEveryStep(ScrollRepeatMode::Steps, false));
    assert(scrollRepeatsEveryStep(ScrollRepeatMode::Steps, true));
  }

  void testActionGrammar() {
    const auto ipc = parseWidgetAction("media toggle");
    assert(ipc.has_value());
    assert(ipc->kind == WidgetAction::Kind::Ipc);
    assert(ipc->verb == "media");
    assert(ipc->args == "toggle");
    assert(ipc->commandLine() == "media toggle");
    assert(ipc->isBound());

    const auto bare = parseWidgetAction("power-cycle");
    assert(bare.has_value());
    assert(bare->verb == "power-cycle");
    assert(bare->args.empty());
    assert(bare->commandLine() == "power-cycle");

    const auto multiArg = parseWidgetAction("  panel-toggle control-center media  ");
    assert(multiArg.has_value());
    assert(multiArg->verb == "panel-toggle");
    assert(multiArg->args == "control-center media");

    const auto exec = parseWidgetAction("exec notify-send hello world");
    assert(exec.has_value());
    assert(exec->kind == WidgetAction::Kind::Exec);
    assert(exec->args == "notify-send hello world");

    const auto none = parseWidgetAction("none");
    assert(none.has_value());
    assert(none->kind == WidgetAction::Kind::None);
    assert(!none->isBound());

    // An unknown verb never falls through to a shell command, and empty is an error rather than a
    // silent synonym for "none".
    assert(!parseWidgetAction("").has_value());
    assert(!parseWidgetAction("   ").has_value());
    assert(!parseWidgetAction("exec").has_value());
    assert(!parseWidgetAction("exec   ").has_value());
    assert(!parseWidgetAction("none please").has_value());
  }

  void testPanelVerbs() {
    assert(isAnchoredPanelVerb("panel-toggle"));
    assert(isAnchoredPanelVerb("panel-open"));
    assert(!isAnchoredPanelVerb("panel-close"));
    assert(!isAnchoredPanelVerb("media"));

    const auto withContext = parsePanelVerbArgs("control-center media");
    assert(withContext.panelId == "control-center");
    assert(withContext.panelContext == "media");

    const auto bare = parsePanelVerbArgs("session");
    assert(bare.panelId == "session");
    assert(bare.panelContext.empty());
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
      assert(bindings.find(Gesture::Middle)->verb == "settings-open-widget");
      assert(bindings.find(Gesture::Left)->verb == "panel-toggle");
      assert(bindings.find(Gesture::Right)->args == "toggle");
      assert(bindings.find(Gesture::Forward) == nullptr);
      assert(!bindings.empty());
      assert(bindings.boundGestures().contains(Gesture::Left));
      assert(!bindings.boundGestures().contains(Gesture::Forward));
    }

    // Layer 2 can unbind a built-in too. This is how a plugin [[widget]] whose script implements
    // onMiddleClick declares that middle belongs to it: nothing bound means the widget's own input
    // area keeps the button.
    {
      constexpr std::array kFreesMiddle{GestureBinding{Gesture::Middle, "none"}};
      WidgetActionBindings bindings;
      bindings.resolve(WidgetActionBindings::Inputs{.builtinDefaults = kBuiltin, .widgetDefaults = kFreesMiddle});
      assert(bindings.find(Gesture::Middle) == nullptr);
      assert(!bindings.boundGestures().contains(Gesture::Middle));
      assert(bindings.empty());
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
      assert(bindings.find(Gesture::Right)->args == "next");
      assert(bindings.find(Gesture::Left) == nullptr);
      assert(bindings.find(Gesture::Middle)->verb == "settings-open-widget");
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
      assert(bindings.find(Gesture::Middle) == nullptr);
      assert(bindings.find(Gesture::Right)->verb == "media");
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
      assert(bindings.find(Gesture::Left) == nullptr);
      assert(bindings.find(Gesture::Middle) != nullptr);
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
      assert(bound != nullptr);
      assert(bound->verb == "medai");
      assert(bound->commandLine() == "medai toggle");
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
      assert(bindings.find(Gesture::Forward)->args == "next");
      assert(bindings.boundGestures().contains(Gesture::Forward));
    }

    // No actions table at all.
    {
      const WidgetConfig empty;
      assert(findActionTable(&empty) == nullptr);
      assert(findActionTable(nullptr) == nullptr);
      WidgetActionBindings bindings;
      bindings.resolve(WidgetActionBindings::Inputs{});
      assert(bindings.empty());
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
    assert(inheritedStep != nullptr);
    assert(inheritedStep->value().empty());
    inheritedStep->setValue("12%");
    inheritedStep->inputArea()->dispatchFocusLoss();
    assert(storedAction == "volume-up 12%");
    assert(storedPath == std::vector<std::string>({"widget", "volume", "actions", "scroll_up"}));

    auto configured = factory.makeGestureActionRow(
        settings::GestureActionSetting{
            .gestureKey = "scroll_up",
            .configured = "volume-up 12%",
            .defaultAction = "volume-up",
        },
        "Scroll up", {"widget", "volume", "actions", "scroll_up"}
    );
    const auto* configuredStep = findInput(*configured);
    assert(configuredStep != nullptr);
    assert(configuredStep->value() == "12%");
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
