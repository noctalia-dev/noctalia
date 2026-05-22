import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Services.Keyboard

Item {
  id: root

  property int floatingWindowPosition: Number.MAX_SAFE_INTEGER

  property ListModel workspaces: ListModel {}
  property var windows: []
  property int focusedWindowIndex: -1

  property bool overviewActive: false
  property bool initialized: false
  property bool globalWorkspaces: false
  property bool connected: false
  property bool stateReady: false

  property var capabilities: ({
                                "event_stream": true,
                                "state": true,
                                "layout_state": true,
                                "overview": true,
                                "workspace_switching": true,
                                "workspace_content_scroll": true,
                                "window_focus": true,
                                "window_close": true,
                                "spawn": true,
                                "keyboard_layout": true,
                                "output_metadata": true,
                                "monitor_power": false,
                                "workspace_urgency": false
                              })
  property bool supportsOverview: capabilities.overview === true
  property bool supportsWorkspaceSwitching: capabilities.workspace_switching === true
  property bool supportsWorkspaceContentScroll: capabilities.workspace_content_scroll === true
  property bool supportsWindowFocus: capabilities.window_focus === true
  property bool supportsWindowClose: capabilities.window_close === true
  property bool supportsSpawn: capabilities.spawn === true
  property bool supportsKeyboardLayout: capabilities.keyboard_layout === true
  property bool supportsMonitorPower: capabilities.monitor_power === true
  property bool supportsWorkspaceUrgency: capabilities.workspace_urgency === true

  property var keyboardLayouts: []
  property var outputCache: ({})
  property var workspaceCache: ({})
  property string capabilitiesSignature: ""
  property string outputSignature: ""
  property string workspaceSignature: ""
  property string windowListSignature: ""
  property string windowFocusSignature: ""
  property string keyboardSignature: ""

  property string socketPath: {
    const configured = Quickshell.env("TRIAD_SOCKET");
    if (configured && configured.length > 0)
      return configured;
    const runtime = Quickshell.env("XDG_RUNTIME_DIR") || "/tmp";
    return runtime + "/triad.sock";
  }

  property var requestQueue: []
  property var activeRequest: null
  property bool requestSent: false
  property bool eventReconnectEnabled: false
  property int requestTimeoutMs: 2000
  property int reconnectAttempt: 0
  property int reconnectBaseMs: 500
  property int reconnectMaxMs: 5000

  signal workspaceChanged
  signal activeWindowChanged
  signal windowListChanged
  signal displayScalesChanged

  function initialize() {
    connectEventStream();
    initialized = true;
    Logger.i("TriadService", "Service started with native IPC socket: " + socketPath);
  }

  function triadRequest(request, extra) {
    const payload = {
      "version": 1,
      "request": request
    };
    if (extra) {
      for (const key in extra) {
        payload[key] = extra[key];
      }
    }
    return JSON.stringify({
                            "triad": payload
                          });
  }

  function triadAction(action, extra) {
    const payload = {
      "version": 1,
      "request": "action",
      "action": action
    };
    if (extra) {
      for (const key in extra) {
        payload[key] = extra[key];
      }
    }
    return JSON.stringify({
                            "triad": payload
                          });
  }

  function sendRequest(payload, callback) {
    requestQueue.push({
                        "payload": payload,
                        "callback": callback || null
                      });
    pumpRequestQueue();
  }

  function pumpRequestQueue() {
    if (activeRequest || requestQueue.length === 0)
      return;

    activeRequest = requestQueue.shift();
    requestSent = false;
    requestTimeout.stop();
    requestSocket.path = socketPath;
    requestSocket.connected = true;
  }

  function finishActiveRequest() {
    requestTimeout.stop();
    activeRequest = null;
    requestSent = false;
    Qt.callLater(root.pumpRequestQueue);
  }

  function refreshState() {
    sendRequest(triadRequest("state"), reply => {
      if (reply && reply.ok && reply.triad && reply.triad.state) {
        applyState(reply.triad.state);
      }
    });
  }

  function refreshCapabilities() {
    sendRequest(triadRequest("capabilities"), reply => {
      if (reply && reply.ok && reply.triad && reply.triad.capabilities) {
        applyCapabilities(reply.triad.capabilities);
      }
    });
  }

  Socket {
    id: requestSocket
    parser: SplitParser {
      splitMarker: "\n"
      onRead: data => {
        if (!root.activeRequest)
          return;
        try {
          const reply = JSON.parse(data);
          if (root.activeRequest.callback)
            root.activeRequest.callback(reply);
          if (!reply.ok)
            Logger.w("TriadService", "Request failed:", data);
        } catch (e) {
          Logger.w("TriadService", "Failed to parse request reply:", e, data);
        }
        requestSocket.connected = false;
      }
    }
    onConnectionStateChanged: {
      if (connected && root.activeRequest && !root.requestSent) {
        root.requestSent = true;
        write(root.activeRequest.payload + "\n");
        flush();
        requestTimeout.restart();
      } else if (!connected && root.activeRequest) {
        root.finishActiveRequest();
      }
    }
    onError: error => {
      Logger.w("TriadService", "Request socket error:", error);
      connected = false;
      root.finishActiveRequest();
    }
  }

  Timer {
    id: requestTimeout
    interval: root.requestTimeoutMs
    repeat: false
    onTriggered: {
      if (!root.activeRequest)
        return;
      Logger.w("TriadService", "Request timed out");
      requestSocket.connected = false;
      root.finishActiveRequest();
    }
  }

  Socket {
    id: eventSocket
    parser: SplitParser {
      splitMarker: "\n"
      onRead: data => root.handleEventLine(data)
    }
    onConnectionStateChanged: {
      if (connected) {
        root.connected = true;
        root.reconnectAttempt = 0;
        write(root.triadRequest("event-stream", {
                                  "events": ["state", "layout", "window"]
                                }) + "\n");
        flush();
      } else if (root.eventReconnectEnabled) {
        root.connected = false;
        root.stateReady = false;
        root.scheduleReconnect();
      }
    }
    onError: error => {
      Logger.w("TriadService", "Event socket error:", error);
      connected = false;
      root.connected = false;
      root.stateReady = false;
      root.scheduleReconnect();
    }
  }

  Timer {
    id: reconnectTimer
    interval: 1000
    repeat: false
    onTriggered: root.connectEventStream()
  }

  function scheduleReconnect() {
    if (reconnectTimer.running)
      return;
    const delay = Math.min(reconnectMaxMs, reconnectBaseMs * Math.pow(2, reconnectAttempt));
    reconnectAttempt = Math.min(reconnectAttempt + 1, 8);
    reconnectTimer.interval = delay;
    reconnectTimer.restart();
  }

  function connectEventStream() {
    root.eventReconnectEnabled = false;
    reconnectTimer.stop();
    if (eventSocket.connected)
      eventSocket.connected = false;
    root.eventReconnectEnabled = true;
    eventSocket.path = socketPath;
    eventSocket.connected = true;
  }

  function handleEventLine(line) {
    if (!line || line.length === 0)
      return;

    try {
      const rootObject = JSON.parse(line);
      if (rootObject.ok !== undefined)
        return;
      if (!rootObject.triad)
        return;

      const event = rootObject.triad.event;
      if (event === "state-changed") {
        applyState(rootObject.triad.state);
      } else if (event === "layout-state-changed") {
        applyLayoutState(rootObject.triad.state);
      } else if (event === "window-changed") {
        applyWindowChanged(rootObject.triad.window);
      }
    } catch (e) {
      Logger.w("TriadService", "Failed to parse event:", e, line);
    }
  }

  Component.onDestruction: {
    eventReconnectEnabled = false;
    reconnectTimer.stop();
    requestTimeout.stop();
    requestQueue = [];
    activeRequest = null;
    requestSent = false;
    if (eventSocket.connected)
      eventSocket.connected = false;
    if (requestSocket.connected)
      requestSocket.connected = false;
  }

  function applyState(state) {
    if (!state)
      return;

    stateReady = true;
    if (state.capabilities)
      applyCapabilities(state.capabilities);
    if (state.outputs)
      applyOutputs(state.outputs);
    if (state.layout)
      applyLayoutState(state.layout);
    if (state.windows)
      applyWindows(state.windows);
    if (state.overview)
      applyOverview(state.overview);
    if (state.keyboard_layouts)
      applyKeyboardLayouts(state.keyboard_layouts, state.current_keyboard_layout_idx || 0);
  }

  function applyCapabilities(nextCapabilities) {
    if (!nextCapabilities)
      return;

    const merged = {};
    for (const key in capabilities) {
      merged[key] = capabilities[key] === true;
    }
    for (const nextKey in nextCapabilities) {
      merged[nextKey] = nextCapabilities[nextKey] === true;
    }
    const signature = JSON.stringify(merged);
    if (signature === capabilitiesSignature)
      return;
    capabilitiesSignature = signature;
    capabilities = merged;
  }

  function applyOverview(overview) {
    const nextActive = overview && overview.is_open === true;
    if (overviewActive !== nextActive)
      overviewActive = nextActive;
  }

  function projectedOutputSignature(output) {
    const geometry = output.geometry || {};
    return [output.name || "", output.connected !== false ? 1 : 0, output.scale || 1.0, geometry.width || 0, geometry.height || 0, geometry.x || 0, geometry.y || 0, output.physical_width || 0, output.physical_height || 0, output.refresh_rate || 60000, output.transform || "Normal"].join(":");
  }

  function applyOutputs(outputs) {
    const nextCache = {};
    const signatureParts = [];
    for (var i = 0; i < outputs.length; i++) {
      const output = outputs[i];
      const geometry = output.geometry || {};
      signatureParts.push(projectedOutputSignature(output));
      nextCache[output.name] = {
        "name": output.name,
        "connected": output.connected !== false,
        "scale": output.scale || 1.0,
        "width": geometry.width || 0,
        "height": geometry.height || 0,
        "x": geometry.x || 0,
        "y": geometry.y || 0,
        "physical_width": output.physical_width || 0,
        "physical_height": output.physical_height || 0,
        "refresh_rate": output.refresh_rate || 60000,
        "vrr_supported": false,
        "vrr_enabled": false,
        "transform": output.transform || "Normal"
      };
    }
    const signature = signatureParts.join("|");
    if (signature === outputSignature)
      return;
    outputSignature = signature;
    windowListSignature = "";
    outputCache = nextCache;
    queryDisplayScales();
  }

  function projectedWorkspaceSignature(ws) {
    return [ws.tag_id, ws.workspace_idx, ws.name || "", ws.output || "", ws.layout || "", ws.layout_kind || "", ws.runtime_kind || "", ws.layout_source || "", ws.fallback_layout || "", ws.is_configured === true ? 1 : 0, ws.is_active === true ? 1 : 0, ws.is_output_visible === true ? 1 : 0, ws.occupied === true ? 1 : 0, ws.is_urgent === true ? 1 : 0, ws.focused_window_id
            || 0].join(":");
  }

  function applyLayoutState(layout) {
    if (!layout || !layout.workspaces)
      return;

    const nextCache = {};
    const workspaceList = [];
    const signatureParts = [];
    for (var i = 0; i < layout.workspaces.length; i++) {
      const ws = layout.workspaces[i];
      const isFocused = ws.is_active === true;
      const isActive = ws.is_output_visible === true;
      const isOccupied = ws.occupied === true;
      const isConfigured = ws.is_configured === true;
      signatureParts.push(projectedWorkspaceSignature(ws));
      const wsData = {
        "id": ws.tag_id,
        "idx": ws.workspace_idx,
        "displayIdx": ws.workspace_idx,
        "triadWorkspaceIdx": ws.workspace_idx,
        "handle": ws.workspace_idx,
        "name": ws.name || "",
        "output": ws.output || "",
        "layout": ws.layout || "",
        "layoutKind": ws.layout_kind || "",
        "runtimeKind": ws.runtime_kind || "",
        "layoutSource": ws.layout_source || "",
        "fallbackLayout": ws.fallback_layout || "",
        "isConfigured": isConfigured,
        "isOutputVisible": ws.is_output_visible === true,
        "isFocused": isFocused,
        "isActive": isActive,
        "isUrgent": supportsWorkspaceUrgency && ws.is_urgent === true,
        "isOccupied": isOccupied
      };
      nextCache[wsData.id] = wsData;
      if (isFocused || isActive || isOccupied || isConfigured)
        workspaceList.push(wsData);
    }

    workspaceList.sort((a, b) => {
      if (a.output !== b.output)
        return a.output.localeCompare(b.output);
      return a.idx - b.idx;
    });
    const displayList = workspaceList.slice().sort((a, b) => a.idx - b.idx);
    for (var displayIndex = 0; displayIndex < displayList.length; displayIndex++) {
      displayList[displayIndex].idx = displayIndex + 1;
      displayList[displayIndex].displayIdx = displayIndex + 1;
    }

    const signature = signatureParts.join("|") + "=>" + workspaceList.map(ws => [ws.id, ws.idx, ws.displayIdx, ws.triadWorkspaceIdx, ws.handle, ws.name, ws.output, ws.layout, ws.layoutKind, ws.runtimeKind, ws.layoutSource, ws.fallbackLayout, ws.isConfigured ? 1 : 0, ws.isFocused ? 1 : 0, ws.isActive ? 1 : 0, ws.isUrgent ? 1 : 0, ws.isOccupied ? 1 : 0].join(
      ":")).join("|");
    if (signature === workspaceSignature)
      return;
    workspaceSignature = signature;
    windowListSignature = "";

    workspaces.clear();
    for (var j = 0; j < workspaceList.length; j++) {
      workspaces.append(workspaceList[j]);
    }

    workspaceCache = nextCache;
    workspaceChanged();
  }

  function toWindowPosition(win) {
    if (win.is_floating) {
      return {
        "x": floatingWindowPosition,
        "y": floatingWindowPosition
      };
    }
    const position = win.position || {};
    return {
      "x": position.column_idx || 0,
      "y": position.window_idx || 0
    };
  }

  function projectedWindowListSignature(win) {
    const position = win.position || {};
    return [win.id, win.title || "", win.app_id || "", win.tag_id || -1, win.output || "", win.is_floating === true ? 1 : 0, win.is_fullscreen === true ? 1 : 0, win.is_floating === true ? floatingWindowPosition : position.column_idx || 0, win.is_floating === true ? floatingWindowPosition : position.window_idx || 0].join(":");
  }

  function projectWindow(win) {
    return {
      "id": win.id,
      "title": win.title || "",
      "appId": win.app_id || "",
      "workspaceId": win.tag_id || -1,
      "isFocused": win.is_focused === true,
      "output": win.output || "",
      "position": toWindowPosition(win),
      "fullscreen": win.is_fullscreen === true,
      "floating": win.is_floating === true
    };
  }

  function applyWindows(snapshotWindows) {
    const windowsList = [];
    const signatureParts = [];
    const focusedIds = [];

    for (var i = 0; i < snapshotWindows.length; i++) {
      const win = snapshotWindows[i];
      signatureParts.push(projectedWindowListSignature(win));
      if (win.is_focused === true)
        focusedIds.push(win.id);
      windowsList.push(projectWindow(win));
    }

    const nextListSignature = signatureParts.join("|");
    const previousFocusSignature = windowFocusSignature;
    const nextFocusSignature = focusedIds.join(",");
    if (nextListSignature === windowListSignature && nextFocusSignature === previousFocusSignature)
      return;

    const sortedWindows = toSortedWindowList(windowsList);
    windows = sortedWindows;
    safeUpdateFocusedWindow();

    if (nextListSignature !== windowListSignature) {
      windowListSignature = nextListSignature;
      windowFocusSignature = nextFocusSignature;
      windowListChanged();
      if (nextFocusSignature !== previousFocusSignature)
        activeWindowChanged();
      return;
    }

    if (nextFocusSignature !== previousFocusSignature) {
      windowFocusSignature = nextFocusSignature;
      activeWindowChanged();
    }
  }

  function applyWindowChanged(win) {
    if (!win || win.id === undefined || win.id === null)
      return;

    const projected = projectWindow(win);
    var existingIndex = -1;
    var wasFocused = false;
    for (var i = 0; i < windows.length; i++) {
      if (windows[i].id === projected.id) {
        existingIndex = i;
        wasFocused = windows[i].isFocused === true;
        break;
      }
    }

    if (existingIndex < 0) {
      refreshState();
      return;
    }

    const nextWindows = windows.slice();
    nextWindows[existingIndex] = projected;
    windows = toSortedWindowList(nextWindows);
    safeUpdateFocusedWindow();
    windowListSignature = "";
    windowFocusSignature = "";
    windowListChanged();
    if (wasFocused || projected.isFocused)
      activeWindowChanged();
  }

  function applyKeyboardLayouts(nextLayouts, currentIndex) {
    const signature = nextLayouts.join("|") + ":" + currentIndex;
    if (signature === keyboardSignature)
      return;
    keyboardSignature = signature;
    keyboardLayouts = nextLayouts;
    const layoutName = keyboardLayouts[currentIndex];
    if (layoutName)
      KeyboardLayoutService.setCurrentLayout(layoutName);
  }

  function toSortedWindowList(windowList) {
    return windowList.map(win => {
      const workspace = workspaceCache[win.workspaceId];
      const output = (workspace && workspace.output) ? outputCache[workspace.output] : outputCache[win.output];
      return {
        "window": win,
        "workspaceIdx": workspace ? workspace.handle || workspace.idx : 0,
        "outputX": output ? output.x : 0,
        "outputY": output ? output.y : 0
      };
    }).sort((a, b) => {
      if (a.outputX !== b.outputX)
        return a.outputX - b.outputX;
      if (a.outputY !== b.outputY)
        return a.outputY - b.outputY;
      if (a.workspaceIdx !== b.workspaceIdx)
        return a.workspaceIdx - b.workspaceIdx;
      if (a.window.position.x !== b.window.position.x)
        return a.window.position.x - b.window.position.x;
      if (a.window.position.y !== b.window.position.y)
        return a.window.position.y - b.window.position.y;
      return a.window.id - b.window.id;
    }).map(info => info.window);
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
    if (!supportsWorkspaceSwitching) {
      Logger.w("TriadService", "Workspace switching is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("focus-workspace", {
                              "workspace_idx": workspace.triadWorkspaceIdx !== undefined ? workspace.triadWorkspaceIdx : (workspace.handle !== undefined ? workspace.handle : workspace.idx)
                            }));
  }

  function scrollWorkspaceContent(direction) {
    if (!supportsWorkspaceContentScroll) {
      Logger.w("TriadService", "Workspace content scrolling is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction(direction < 0 ? "focus-column-left" : "focus-column-right"));
  }

  function focusWindow(window) {
    if (!supportsWindowFocus) {
      Logger.w("TriadService", "Window focus is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("focus-window", {
                              "id": window.id
                            }));
  }

  function closeWindow(window) {
    if (!supportsWindowClose) {
      Logger.w("TriadService", "Window close is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("close-window", {
                              "id": window.id
                            }));
  }

  function turnOffMonitors() {
    if (!supportsMonitorPower) {
      Logger.w("TriadService", "Monitor power off is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("power-off-monitors"));
  }

  function turnOnMonitors() {
    if (!supportsMonitorPower) {
      Logger.w("TriadService", "Monitor power on is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("power-on-monitors"));
  }

  function logout() {
    sendRequest(triadAction("exit-session"));
  }

  function cycleKeyboardLayout() {
    if (!supportsKeyboardLayout) {
      Logger.w("TriadService", "Keyboard layout switching is not exposed by native Triad IPC");
      return;
    }
    sendRequest(triadAction("switch-keyboard-layout", {
                              "layout": "next"
                            }));
  }

  function getFocusedScreen() {
    for (var i = 0; i < workspaces.count; i++) {
      const ws = workspaces.get(i);
      if (!ws.isFocused)
        continue;
      for (var j = 0; j < Quickshell.screens.length; j++) {
        if (Quickshell.screens[j].name === ws.output)
          return Quickshell.screens[j];
      }
    }
    return null;
  }

  function spawn(command) {
    if (!supportsSpawn) {
      Logger.w("TriadService", "Spawning commands is not exposed by native Triad IPC");
      return;
    }
    const argv = Array.isArray(command) ? command : Array.from(command || []);
    if (argv.length === 0)
      return;
    sendRequest(triadAction("spawn", {
                              "argv": argv
                            }));
  }
}
