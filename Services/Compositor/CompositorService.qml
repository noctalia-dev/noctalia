pragma Singleton

import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Services.Control
import qs.Services.UI

Singleton {
  id: root

  // Compositor detection
  property bool isHyprland: false
  property bool isNiri: false
  property bool isSway: false
  property bool isMango: false
  property bool isTriad: false
  property bool isLabwc: false
  property bool isExtWorkspace: false
  property bool isScroll: false

  // Generic workspace and window data
  property ListModel workspaces: ListModel {}
  property ListModel windows: ListModel {}
  property int focusedWindowIndex: -1

  // Display scale data
  property var displayScales: ({})
  property bool displayScalesLoaded: false

  // Overview state (Niri-specific, defaults to false for other compositors)
  property bool overviewActive: false
  property bool supportsOverview: isNiri || (backend !== null && backend.supportsOverview === true)
  property bool supportsWorkspaceSwitching: backend !== null && backend.switchToWorkspace !== undefined && (backend.supportsWorkspaceSwitching === undefined || backend.supportsWorkspaceSwitching === true)
  property bool supportsWorkspaceContentScroll: isNiri || (backend !== null && backend.supportsWorkspaceContentScroll === true)
  property bool supportsWindowFocus: backend !== null && backend.focusWindow !== undefined && (backend.supportsWindowFocus === undefined || backend.supportsWindowFocus === true)
  property bool supportsWindowClose: backend !== null && backend.closeWindow !== undefined && (backend.supportsWindowClose === undefined || backend.supportsWindowClose === true)
  property bool supportsSpawn: backend !== null && backend.spawn !== undefined && (backend.supportsSpawn === undefined || backend.supportsSpawn === true)
  property bool supportsMonitorPower: backend !== null && backend.turnOffMonitors !== undefined && (backend.supportsMonitorPower === undefined || backend.supportsMonitorPower === true)
  property bool supportsKeyboardLayout: backend !== null && backend.cycleKeyboardLayout !== undefined && (backend.supportsKeyboardLayout === undefined || backend.supportsKeyboardLayout === true)

  // Global workspaces flag (workspaces shared across all outputs)
  // True for LabWC (stacking compositor), false for tiling WMs with per-output workspaces
  property bool globalWorkspaces: false

  // Generic events
  signal workspaceChanged
  signal activeWindowChanged
  signal windowListChanged

  // Backend service loader
  property var backend: null

  Component.onCompleted: {
    // Load display scales from ShellState
    Qt.callLater(() => {
                   if (typeof ShellState !== 'undefined' && ShellState.isLoaded) {
                     loadDisplayScalesFromState();
                   }
                 });

    detectCompositor();
  }

  Connections {
    target: typeof ShellState !== 'undefined' ? ShellState : null
    function onIsLoadedChanged() {
      if (ShellState.isLoaded) {
        loadDisplayScalesFromState();
      }
    }
  }

  function detectCompositor() {
    const hyprlandSignature = Quickshell.env("HYPRLAND_INSTANCE_SIGNATURE");
    const niriSocket = Quickshell.env("NIRI_SOCKET");
    const triadSocket = Quickshell.env("TRIAD_SOCKET");
    const swaySock = Quickshell.env("SWAYSOCK");
    const currentDesktop = Quickshell.env("XDG_CURRENT_DESKTOP");
    const sessionDesktop = Quickshell.env("XDG_SESSION_DESKTOP");
    const desktopSession = Quickshell.env("DESKTOP_SESSION");
    const riverWm = Quickshell.env("RIVER_WM");
    const labwcPid = Quickshell.env("LABWC_PID");
    const triadDetected = (triadSocket && triadSocket.length > 0) || (riverWm && riverWm.toLowerCase() === "triad") || (currentDesktop && currentDesktop.toLowerCase().includes("triad")) || (sessionDesktop && sessionDesktop.toLowerCase().includes("triad")) || (desktopSession && desktopSession.toLowerCase().includes("triad"));

    // Check for MangoWC using XDG_CURRENT_DESKTOP environment variable
    // MangoWC sets XDG_CURRENT_DESKTOP=mango
    if (triadDetected) {
      isHyprland = false;
      isNiri = false;
      isSway = false;
      isMango = false;
      isTriad = true;
      isLabwc = false;
      isExtWorkspace = false;
      backendLoader.sourceComponent = triadComponent;
    } else if (currentDesktop && currentDesktop.toLowerCase().includes("mango")) {
      isHyprland = false;
      isNiri = false;
      isSway = false;
      isMango = true;
      isTriad = false;
      isLabwc = false;
      isExtWorkspace = false;
      backendLoader.sourceComponent = mangoComponent;
    } else if (labwcPid && labwcPid.length > 0) {
      isHyprland = false;
      isNiri = false;
      isSway = false;
      isMango = false;
      isTriad = false;
      isLabwc = true;
      isExtWorkspace = false;
      backendLoader.sourceComponent = labwcComponent;
      Logger.i("CompositorService", "Detected LabWC with PID: " + labwcPid);
    } else if (niriSocket && niriSocket.length > 0) {
      isHyprland = false;
      isNiri = true;
      isSway = false;
      isMango = false;
      isTriad = false;
      isLabwc = false;
      isExtWorkspace = false;
      backendLoader.sourceComponent = niriComponent;
    } else if (hyprlandSignature && hyprlandSignature.length > 0) {
      isHyprland = true;
      isNiri = false;
      isSway = false;
      isMango = false;
      isTriad = false;
      isLabwc = false;
      isExtWorkspace = false;
      backendLoader.sourceComponent = hyprlandComponent;
    } else if (swaySock && swaySock.length > 0) {
      isHyprland = false;
      isNiri = false;
      isSway = true;
      isMango = false;
      isTriad = false;
      isLabwc = false;
      isExtWorkspace = false;
      isScroll = currentDesktop && currentDesktop.toLowerCase().includes("scroll");
      backendLoader.sourceComponent = swayComponent;
    } else {
      // Always fallback to ext-workspace-v1
      isHyprland = false;
      isNiri = false;
      isSway = false;
      isMango = false;
      isTriad = false;
      isLabwc = false;
      isExtWorkspace = true;
      backendLoader.sourceComponent = extWorkspaceComponent;
      Logger.i("CompositorService", "Using generic ext-workspace backend (no recognized compositor env)");
    }
  }

  Loader {
    id: backendLoader
    onLoaded: {
      if (item) {
        if (isScroll) {
          item.msgCommand = "scrollmsg";
        }
        root.backend = item;
        setupBackendConnections();
        backend.initialize();
      }
    }
  }

  // Load display scales from ShellState
  function loadDisplayScalesFromState() {
    try {
      const cached = ShellState.getDisplay();
      if (cached && Object.keys(cached).length > 0) {
        displayScales = cached;
        displayScalesLoaded = true;
        Logger.d("CompositorService", "Loaded display scales from ShellState");
      } else {
        // Migration is now handled in Settings.qml
        displayScalesLoaded = true;
      }
    } catch (error) {
      Logger.e("CompositorService", "Failed to load display scales:", error);
      displayScalesLoaded = true;
    }
  }

  // Hyprland backend component
  Component {
    id: hyprlandComponent
    HyprlandService {
      id: hyprlandBackend
    }
  }

  // Niri backend component
  Component {
    id: niriComponent
    NiriService {
      id: niriBackend
    }
  }

  // Sway backend component
  Component {
    id: swayComponent
    SwayService {
      id: swayBackend
    }
  }

  // Mango backend component
  Component {
    id: mangoComponent
    MangoService {
      id: mangoBackend
    }
  }

  // Triad backend component
  Component {
    id: triadComponent
    TriadService {
      id: triadBackend
    }
  }

  // Labwc backend component
  Component {
    id: labwcComponent
    LabwcService {
      id: labwcBackend
    }
  }

  // Generic ext-workspace (WindowManager) when compositor env is unknown
  Component {
    id: extWorkspaceComponent
    ExtWorkspaceService {
      id: extWorkspaceBackend
    }
  }

  function setupBackendConnections() {
    if (!backend)
      return;

    // Connect backend signals to facade signals
    backend.workspaceChanged.connect(() => {
                                       // Sync workspaces when they change
                                       syncWorkspaces();
                                       // Forward the signal
                                       workspaceChanged();
                                     });

    backend.activeWindowChanged.connect(() => {
                                          // Only sync focus state, not entire window list
                                          syncFocusedWindow();
                                          // Forward the signal
                                          activeWindowChanged();
                                        });

    backend.windowListChanged.connect(() => {
                                        syncWindows();
                                      });

    // Property bindings - use automatic property change signal
    backend.focusedWindowIndexChanged.connect(() => {
                                                focusedWindowIndex = backend.focusedWindowIndex;
                                              });

    // Overview state (Niri-specific)
    if (backend.overviewActiveChanged) {
      backend.overviewActiveChanged.connect(() => {
                                              overviewActive = backend.overviewActive;
                                            });
    }

    // Initial sync
    syncWorkspaces();
    syncWindows();
    focusedWindowIndex = backend.focusedWindowIndex;
    if (backend.overviewActive !== undefined) {
      overviewActive = backend.overviewActive;
    }
    if (backend.globalWorkspaces !== undefined) {
      globalWorkspaces = backend.globalWorkspaces;
    }
  }

  function syncWorkspaces() {
    workspaces.clear();
    const ws = backend.workspaces;
    for (var i = 0; i < ws.count; i++) {
      workspaces.append(ws.get(i));
    }
    // Emit signal to notify listeners that workspace list has been updated
    workspacesChanged();
  }

  function syncWindows() {
    windows.clear();
    const ws = backend.windows;
    for (var i = 0; i < ws.length; i++) {
      windows.append(ws[i]);
    }
    // Emit signal to notify listeners that window list has been updated
    windowListChanged();
  }

  // Sync only the focused window state, not the entire window list
  function syncFocusedWindow() {
    const newIndex = backend.focusedWindowIndex;

    // Update isFocused flags by syncing from backend
    for (var i = 0; i < windows.count && i < backend.windows.length; i++) {
      const backendFocused = backend.windows[i].isFocused;
      if (windows.get(i).isFocused !== backendFocused) {
        windows.setProperty(i, "isFocused", backendFocused);
      }
    }

    focusedWindowIndex = newIndex;
  }

  // Update display scales from backend
  function updateDisplayScales() {
    if (!backend || !backend.queryDisplayScales) {
      Logger.w("CompositorService", "Backend does not support display scale queries");
      return;
    }

    backend.queryDisplayScales();
  }

  // Called by backend when display scales are ready
  function onDisplayScalesUpdated(scales) {
    displayScales = scales;
    saveDisplayScalesToCache();
    Logger.d("CompositorService", "Display scales updated");
  }

  // Save display scales to cache
  function saveDisplayScalesToCache() {
    try {
      ShellState.setDisplay(displayScales);
      Logger.d("CompositorService", "Saved display scales to ShellState");
    } catch (error) {
      Logger.e("CompositorService", "Failed to save display scales:", error);
    }
  }

  // Public function to get scale for a specific display
  function getDisplayScale(displayName) {
    if (!displayName || !displayScales[displayName]) {
      return 1.0;
    }
    return displayScales[displayName].scale || 1.0;
  }

  // Public function to get all display info for a specific display
  function getDisplayInfo(displayName) {
    if (!displayName || !displayScales[displayName]) {
      return null;
    }
    return displayScales[displayName];
  }

  // Get focused window
  function getFocusedWindow() {
    if (focusedWindowIndex >= 0 && focusedWindowIndex < windows.count) {
      return windows.get(focusedWindowIndex);
    }
    return null;
  }

  // Get focused screen from compositor
  function getFocusedScreen() {
    if (backend && backend.getFocusedScreen) {
      return backend.getFocusedScreen();
    }
    return null;
  }

  // Get focused window title
  function getFocusedWindowTitle() {
    if (focusedWindowIndex >= 0 && focusedWindowIndex < windows.count) {
      var title = windows.get(focusedWindowIndex).title;
      if (title !== undefined) {
        title = title.replace(/(\r\n|\n|\r)/g, "");
      }
      return title || "";
    }
    return "";
  }

  // Get clean app name from appId
  // Extracts the last segment from reverse domain notation (e.g., "org.kde.dolphin" -> "Dolphin")
  // Falls back to title if appId is empty
  function getCleanAppName(appId, fallbackTitle) {
    var name = (appId || "").split(".").pop() || fallbackTitle || "Unknown";
    return name.charAt(0).toUpperCase() + name.slice(1);
  }

  function getWindowsForWorkspace(workspaceId) {
    var windowsInWs = [];
    for (var i = 0; i < windows.count; i++) {
      var window = windows.get(i);
      if (window.workspaceId === workspaceId) {
        // Snapshot to plain JS object so callers never hold live ListModel
        // proxies that become invalid when syncWindows() clears the model.
        windowsInWs.push({
                           id: window.id,
                           title: window.title,
                           appId: window.appId,
                           isFocused: window.isFocused,
                           workspaceId: window.workspaceId,
                           handle: window.handle
                         });
      }
    }
    return windowsInWs;
  }

  // Generic workspace switching
  function switchToWorkspace(workspace) {
    if (!supportsWorkspaceSwitching) {
      Logger.w("Compositor", "Workspace switching is not supported by the active backend");
      return;
    }
    if (backend && backend.switchToWorkspace) {
      backend.switchToWorkspace(workspace);
    } else {
      Logger.w("Compositor", "No backend available for workspace switching");
    }
  }

  // Scrollable workspace content (Niri)
  function scrollWorkspaceContent(direction) {
    if (!supportsWorkspaceContentScroll) {
      Logger.w("Compositor", "Workspace content scrolling is not supported by the active backend");
      return;
    }
    if (backend && backend.scrollWorkspaceContent) {
      backend.scrollWorkspaceContent(direction);
    }
  }

  // Get current workspace
  function getCurrentWorkspace() {
    for (var i = 0; i < workspaces.count; i++) {
      const ws = workspaces.get(i);
      if (ws.isFocused) {
        return ws;
      }
    }
    return null;
  }

  // Get active workspaces
  function getActiveWorkspaces() {
    const activeWorkspaces = [];
    for (var i = 0; i < workspaces.count; i++) {
      const ws = workspaces.get(i);
      if (ws.isActive) {
        activeWorkspaces.push(ws);
      }
    }
    return activeWorkspaces;
  }

  // Set focused window
  function focusWindow(window) {
    if (!supportsWindowFocus) {
      Logger.w("Compositor", "Window focus is not supported by the active backend");
      return;
    }
    if (backend && backend.focusWindow) {
      backend.focusWindow(window);
    } else {
      Logger.w("Compositor", "No backend available for window focus");
    }
  }

  // Close window
  function closeWindow(window) {
    if (!supportsWindowClose) {
      Logger.w("Compositor", "Window closing is not supported by the active backend");
      return;
    }
    if (backend && backend.closeWindow) {
      backend.closeWindow(window);
    } else {
      Logger.w("Compositor", "No backend available for window closing");
    }
  }

  // Spawn command
  function spawn(command) {
    // Ensure command is a proper JS array (QML lists can behave unexpectedly in some contexts)
    const cmdArray = Array.isArray(command) ? command : (command && typeof command === "object" && command.length !== undefined) ? Array.from(command) : [command];

    Logger.d("CompositorService", `Spawning: ${cmdArray.join(" ")}`);
    if (supportsSpawn && backend && backend.spawn) {
      backend.spawn(cmdArray);
    } else {
      try {
        Quickshell.execDetached(cmdArray);
      } catch (e) {
        Logger.e("CompositorService", "Failed to execute detached:", e);
      }
    }
  }

  // Session management helper for custom commands
  function getCustomCommand(action) {
    const powerOptions = Settings.data.sessionMenu.powerOptions || [];
    for (let i = 0; i < powerOptions.length; i++) {
      const option = powerOptions[i];
      if (option.action === action && option.enabled && option.command && option.command.trim() !== "") {
        return option.command.trim();
      }
    }
    return "";
  }

  function executeSessionAction(action, defaultCommand) {
    const customCommand = getCustomCommand(action);
    if (customCommand) {
      Logger.i("Compositor", `Executing custom command for action: ${action} Command: ${customCommand}`);
      Quickshell.execDetached(["sh", "-c", customCommand]);
      return true;
    }
    return false;
  }

  // Session management
  function logout() {
    Logger.i("Compositor", "Logout requested");
    if (executeSessionAction("logout"))
      return;

    if (backend && backend.logout) {
      backend.logout();
    } else {
      Logger.w("Compositor", "No backend available for logout");
    }
  }

  function shutdown() {
    Logger.i("Compositor", "Shutdown requested");
    if (executeSessionAction("shutdown"))
      return;

    HooksService.executeSessionHook("shutdown", () => {
                                      Quickshell.execDetached(["sh", "-c", "systemctl poweroff || loginctl poweroff"]);
                                    });
  }

  function reboot() {
    Logger.i("Compositor", "Reboot requested");
    if (executeSessionAction("reboot"))
      return;

    HooksService.executeSessionHook("reboot", () => {
                                      Quickshell.execDetached(["sh", "-c", "systemctl reboot || loginctl reboot"]);
                                    });
  }

  function userspaceReboot() {
    Logger.i("Compositor", "Userspace reboot requested");
    if (executeSessionAction("userspaceReboot"))
      return;

    HooksService.executeSessionHook("userspaceReboot", () => {
                                      Quickshell.execDetached(["sh", "-c", "systemctl soft-reboot"]);
                                    });
  }

  function rebootToUefi() {
    Logger.i("Compositor", "Reboot to UEFI firmware requested requested");
    if (executeSessionAction("rebootToUefi"))
      return;

    HooksService.executeSessionHook("rebootToUefi", () => {
                                      Quickshell.execDetached(["sh", "-c", "systemctl reboot --firmware-setup || loginctl reboot --firmware-setup"]);
                                    });
  }

  function turnOffMonitors() {
    Logger.i("Compositor", "Turn off monitors requested");
    if (!supportsMonitorPower) {
      Logger.w("Compositor", "Monitor power control is not supported by the active backend");
      return;
    }
    if (backend && backend.turnOffMonitors) {
      backend.turnOffMonitors();
    } else {
      Logger.w("Compositor", "No backend available for turnOffMonitors");
    }
  }

  function turnOnMonitors() {
    Logger.i("Compositor", "Turn on monitors requested");
    if (!supportsMonitorPower) {
      Logger.w("Compositor", "Monitor power control is not supported by the active backend");
      return;
    }
    if (backend && backend.turnOnMonitors) {
      backend.turnOnMonitors();
    } else {
      Logger.w("Compositor", "No backend available for turnOnMonitors");
    }
  }

  function suspend() {
    Logger.i("Compositor", "Suspend requested");
    if (executeSessionAction("suspend"))
      return;

    Quickshell.execDetached(["sh", "-c", "systemctl suspend || loginctl suspend"]);
  }

  function lock() {
    Logger.i("Compositor", "LockScreen requested");
    if (executeSessionAction("lock"))
      return;

    if (PanelService && PanelService.lockScreen) {
      PanelService.lockScreen.active = true;
    }
  }

  function hibernate() {
    Logger.i("Compositor", "Hibernate requested");
    if (executeSessionAction("hibernate"))
      return;

    Quickshell.execDetached(["sh", "-c", "systemctl hibernate || loginctl hibernate"]);
  }

  function cycleKeyboardLayout() {
    if (!supportsKeyboardLayout)
      return;
    if (backend && backend.cycleKeyboardLayout) {
      backend.cycleKeyboardLayout();
    }
  }

  property int lockAndSuspendCheckCount: 0

  function lockAndSuspend() {
    Logger.i("Compositor", "Lock and suspend requested");

    // if a custom lock command exists, execute it and suspend without wait
    if (executeSessionAction("lock")) {
      suspend();
      return;
    }

    // If already locked, suspend immediately
    if (PanelService && PanelService.lockScreen && PanelService.lockScreen.active) {
      Logger.i("Compositor", "Screen already locked, suspending");
      suspend();
      return;
    }

    // Lock the screen first
    try {
      if (PanelService && PanelService.lockScreen) {
        PanelService.lockScreen.active = true;
        lockAndSuspendCheckCount = 0;

        // Wait for lock screen to be confirmed active before suspending
        lockAndSuspendTimer.start();
      } else {
        Logger.w("Compositor", "Lock screen not available, suspending without lock");
        suspend();
      }
    } catch (e) {
      Logger.w("Compositor", "Failed to activate lock screen before suspend: " + e);
      suspend();
    }
  }

  Timer {
    id: lockAndSuspendTimer
    interval: 100
    repeat: true
    running: false

    onTriggered: {
      lockAndSuspendCheckCount++;

      // Check if lock screen is now active
      if (PanelService && PanelService.lockScreen && PanelService.lockScreen.active) {
        // Verify the lock screen component is loaded
        if (PanelService.lockScreen.item) {
          Logger.i("Compositor", "Lock screen confirmed active, suspending");
          stop();
          lockAndSuspendCheckCount = 0;
          suspend();
        } else {
          // Lock screen is active but component not loaded yet, wait a bit more
          if (lockAndSuspendCheckCount > 20) {
            // Max 2 seconds wait
            Logger.w("Compositor", "Lock screen active but component not loaded, suspending anyway");
            stop();
            lockAndSuspendCheckCount = 0;
            suspend();
          }
        }
      } else {
        // Lock screen not active yet, keep checking
        if (lockAndSuspendCheckCount > 30) {
          // Max 3 seconds wait
          Logger.w("Compositor", "Lock screen failed to activate, suspending anyway");
          stop();
          lockAndSuspendCheckCount = 0;
          suspend();
        }
      }
    }
  }
}
