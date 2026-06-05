import QtQuick
import Quickshell
import Quickshell.Niri
import qs.Commons
import qs.Services.Keyboard

Item {
  id: root

  property int floatingWindowPosition: Number.MAX_SAFE_INTEGER

  property ListModel workspaces: ListModel {}
  property var windows: []
  property int focusedWindowIndex: -1

  property bool overviewActive: false

  property var keyboardLayouts: []

  signal workspaceChanged
  signal activeWindowChanged
  signal windowListChanged
  signal displayScalesChanged

  property var outputCache: ({})
  property var workspaceCache: ({})

  function initialize() {
    Niri.refreshOutputs();
    Niri.refreshWorkspaces();
    Niri.refreshWindows();

    Qt.callLater(() => {
                   safeUpdateOutputs();
                   safeUpdateWorkspaces();
                   safeUpdateWindows();
                   queryDisplayScales();
                 });

    Logger.i("NiriService", "Service started");
  }

  // Connections to the C++ Niri IPC module
  Connections {
    target: Niri
    function onWorkspacesUpdated() {
      safeUpdateWorkspaces();
      workspaceChanged();
    }
    function onWindowsUpdated() {
      safeUpdateWindows();
      windowListChanged();
      activeWindowChanged();
    }
    function onOutputsUpdated() {
      safeUpdateOutputs();
      queryDisplayScales();
    }
    function onOverviewActiveChanged() {
      overviewActive = Niri.overviewActive;
    }
    function onKeyboardLayoutsChanged() {
      keyboardLayouts = Niri.keyboardLayoutNames;
      KeyboardLayoutService.setAllLayouts(keyboardLayouts);
      const layoutName = Niri.currentKeyboardLayoutName;
      if (layoutName) {
        KeyboardLayoutService.setCurrentLayout(layoutName);
      }
      Logger.d("NiriService", "Keyboard layouts changed:", keyboardLayouts.toString());
    }
    function onKeyboardLayoutSwitched() {
      const layoutName = Niri.currentKeyboardLayoutName;
      if (layoutName) {
        KeyboardLayoutService.setCurrentLayout(layoutName);
      }
      Logger.d("NiriService", "Keyboard layout switched:", layoutName);
    }
  }

  function safeUpdateOutputs() {
    const niriOutputs = Niri.outputs.values;
    outputCache = {};

    for (var i = 0; i < niriOutputs.length; i++) {
      const output = niriOutputs[i];
      outputCache[output.name] = {
        "name": output.name,
        "connected": output.connected,
        "scale": output.scale,
        "width": output.width,
        "height": output.height,
        "x": output.x,
        "y": output.y,
        "physical_width": output.physicalWidth,
        "physical_height": output.physicalHeight,
        "refresh_rate": output.refreshRate,
        "vrr_supported": output.vrrSupported,
        "vrr_enabled": output.vrrEnabled,
        "transform": output.transform
      };
    }
  }

  function safeUpdateWorkspaces() {
    const niriWorkspaces = Niri.workspaces.values;
    workspaceCache = {};

    const workspacesList = [];
    for (var i = 0; i < niriWorkspaces.length; i++) {
      const ws = niriWorkspaces[i];
      const wsData = {
        "id": ws.id,
        "idx": ws.idx,
        "name": ws.name,
        "output": ws.output,
        "isFocused": ws.focused,
        "isActive": ws.active,
        "isUrgent": ws.urgent,
        "isOccupied": ws.occupied
      };
      workspacesList.push(wsData);
      workspaceCache[ws.id] = wsData;
    }

    // Workspaces come pre-sorted from C++ (by output then idx)
    workspaces.clear();
    for (var j = 0; j < workspacesList.length; j++) {
      workspaces.append(workspacesList[j]);
    }
  }

  function getWindowOutput(win) {
    for (var i = 0; i < workspaces.count; i++) {
      if (workspaces.get(i).id === win.workspaceId) {
        return workspaces.get(i).output;
      }
    }
    return null;
  }

  function toSortedWindowList(windowList) {
    return windowList.map(win => {
                            const workspace = workspaceCache[win.workspaceId];
                            const output = (workspace && workspace.output) ? outputCache[workspace.output] : null;

                            return {
                              window: win,
                              workspaceIdx: workspace ? workspace.idx : 0,
                              outputX: output ? output.x : 0,
                              outputY: output ? output.y : 0
                            };
                          }).sort((a, b) => {
                                    // Sort by output position first
                                    if (a.outputX !== b.outputX) {
                                      return a.outputX - b.outputX;
                                    }
                                    if (a.outputY !== b.outputY) {
                                      return a.outputY - b.outputY;
                                    }
                                    // Then by workspace index
                                    if (a.workspaceIdx !== b.workspaceIdx) {
                                      return a.workspaceIdx - b.workspaceIdx;
                                    }
                                    // Then by window position
                                    if (a.window.position.x !== b.window.position.x) {
                                      return a.window.position.x - b.window.position.x;
                                    }
                                    if (a.window.position.y !== b.window.position.y) {
                                      return a.window.position.y - b.window.position.y;
                                    }
                                    // Finally by window ID to ensure consistent ordering
                                    return a.window.id - b.window.id;
                                  }).map(info => info.window);
  }

  function safeUpdateWindows() {
    const niriWindows = Niri.windows.values;
    const windowsList = [];

    for (var i = 0; i < niriWindows.length; i++) {
      const win = niriWindows[i];
      windowsList.push({
                         "id": win.id,
                         "title": win.title || "",
                         "appId": win.appId || "",
                         "workspaceId": win.workspaceId || -1,
                         "isFocused": win.focused,
                         "output": win.output || getWindowOutput(win) || "",
                         "position": {
                           "x": win.isFloating ? floatingWindowPosition : win.positionX,
                           "y": win.isFloating ? floatingWindowPosition : win.positionY
                         }
                       });
    }

    windows = toSortedWindowList(windowsList);
    safeUpdateFocusedWindow();
  }

  function safeUpdateFocusedWindow() {
    focusedWindowIndex = -1;
    for (var i = 0; i < windows.length; i++) {
      if (windows[i].isFocused) {
        focusedWindowIndex = i;
        break;
      }
    }
  }

  function queryDisplayScales() {
    if (CompositorService && CompositorService.onDisplayScalesUpdated) {
      CompositorService.onDisplayScalesUpdated(outputCache);
    }
  }

  function switchToWorkspace(workspace) {
    try {
      Niri.dispatch(["focus-workspace", workspace.idx.toString()]);
    } catch (e) {
      Logger.e("NiriService", "Failed to switch workspace:", e);
    }
  }

  function scrollWorkspaceContent(direction) {
    try {
      var action = direction < 0 ? "focus-column-left" : "focus-column-right";
      Niri.dispatch([action]);
    } catch (e) {
      Logger.e("NiriService", "Failed to scroll workspace content:", e);
    }
  }

  function focusWindow(window) {
    try {
      Niri.dispatch(["focus-window", "--id", window.id.toString()]);
    } catch (e) {
      Logger.e("NiriService", "Failed to switch window:", e);
    }
  }

  function closeWindow(window) {
    try {
      Niri.dispatch(["close-window", "--id", window.id.toString()]);
    } catch (e) {
      Logger.e("NiriService", "Failed to close window:", e);
    }
  }

  function turnOffMonitors() {
    try {
      Niri.dispatch(["power-off-monitors"]);
    } catch (e) {
      Logger.e("NiriService", "Failed to turn off monitors:", e);
    }
  }

  function turnOnMonitors() {
    try {
      Niri.dispatch(["power-on-monitors"]);
    } catch (e) {
      Logger.e("NiriService", "Failed to turn on monitors:", e);
    }
  }

  function logout() {
    try {
      Niri.dispatch(["quit", "--skip-confirmation"]);
    } catch (e) {
      Logger.e("NiriService", "Failed to logout:", e);
    }
  }

  function cycleKeyboardLayout() {
    try {
      Niri.dispatch(["switch-layout", "next"]);
    } catch (e) {
      Logger.e("NiriService", "Failed to cycle keyboard layout:", e);
    }
  }

  function getFocusedScreen() {
    // On niri the code below only works when you have an actual app selected on that screen.
    return null;
  }

  function spawn(command) {
    try {
      const niriArgs = ["spawn", "--"].concat(command);
      Logger.d("NiriService", "Calling niri spawn: niri msg action " + niriArgs.join(" "));
      Niri.dispatch(niriArgs);
    } catch (e) {
      Logger.e("NiriService", "Failed to spawn command:", e);
    }
  }
}
