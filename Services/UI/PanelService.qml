pragma Singleton

import QtQuick
import Quickshell
import qs.Commons
import qs.Services.Compositor

Singleton {
  id: root

  // A ref. to the lockScreen, so it's accessible from anywhere.
  property var lockScreen: null

  // Panels
  property var registeredPanels: ({})
  property var openedPanel: null
  property var closingPanel: null
  property bool closedImmediately: false

  // Overlay launcher state (separate from normal panels)
  property bool overlayLauncherOpen: false
  property var overlayLauncherScreen: null
  property var overlayLauncherCore: null  // Reference to LauncherCore when overlay is active
  // Brief window after panel opens where Exclusive keyboard is allowed on Hyprland
  // This allows text inputs to receive focus, then switches to OnDemand for click-to-close
  property bool isInitializingKeyboard: false

  // Global state for keybind recording components to block global shortcuts
  property bool isKeybindRecording: false

  // Hyprland refocus workaround: id (address) of the toplevel that held keyboard
  // focus before a panel opened. When the last panel closes, focus is handed back
  // to it explicitly, because Hyprland (with follow_mouse != 1) leaves the app
  // "active" but unable to receive keyboard input after a still-mapped layer
  // surface releases keyboard focus. See Hyprland #14285 / #14730 and the
  // DankMaterialShell fix (PR #2655); noctalia's persistent MainScreen surface
  // can't release-on-unmap like a per-popout surface, so it nudges focus instead.
  property string windowIdToRestoreFocus: ""

  signal willOpen
  signal didClose

  // Background slot assignments for dynamic panel background rendering
  // Slot 0: currently opening/open panel, Slot 1: closing panel
  property var backgroundSlotAssignments: [null, null]
  signal slotAssignmentChanged(int slotIndex, var panel)

  function assignToSlot(slotIndex, panel) {
    if (backgroundSlotAssignments[slotIndex] !== panel) {
      var newAssignments = backgroundSlotAssignments.slice();
      newAssignments[slotIndex] = panel;
      backgroundSlotAssignments = newAssignments;
      slotAssignmentChanged(slotIndex, panel);
    }
  }

  // Popup menu windows (one per screen) - used for both tray menus and context menus
  property var popupMenuWindows: ({})
  signal popupMenuWindowRegistered(var screen)

  // Register this panel (called after panel is loaded)
  function registerPanel(panel) {
    registeredPanels[panel.objectName] = panel;
    Logger.d("PanelService", "Registered panel:", panel.objectName);
  }

  // Register popup menu window for a screen
  function registerPopupMenuWindow(screen, window) {
    if (!screen || !window)
      return;
    var key = screen.name;
    popupMenuWindows[key] = window;
    Logger.d("PanelService", "Registered popup menu window for screen:", key);
    popupMenuWindowRegistered(screen);
  }

  // Unregister popup menu window for a screen (called on destruction)
  function unregisterPopupMenuWindow(screen) {
    if (!screen)
      return;
    var key = screen.name;
    delete popupMenuWindows[key];
    Logger.d("PanelService", "Unregistered popup menu window for screen:", key);
  }

  // Get popup menu window for a screen
  function getPopupMenuWindow(screen) {
    if (!screen)
      return null;
    return popupMenuWindows[screen.name] || null;
  }

  // Show a context menu with proper handling for all compositors
  // Optional targetItem: if provided, menu will be horizontally centered on this item instead of anchorItem
  function showContextMenu(contextMenu, anchorItem, screen, targetItem) {
    if (!contextMenu || !anchorItem)
      return;

    // Close any previously opened context menu first
    closeContextMenu(screen);

    var popupMenuWindow = getPopupMenuWindow(screen);
    if (popupMenuWindow) {
      popupMenuWindow.showContextMenu(contextMenu);
      contextMenu.openAtItem(anchorItem, screen, targetItem);
    }
  }

  // Close any open context menu or popup menu window
  function closeContextMenu(screen) {
    var popupMenuWindow = getPopupMenuWindow(screen);
    if (popupMenuWindow && popupMenuWindow.visible) {
      popupMenuWindow.close();
    }
  }

  // Show a tray menu with proper handling for all compositors
  // Returns true if menu was shown successfully
  function showTrayMenu(screen, trayItem, trayMenu, anchorItem, menuX, menuY, widgetSection, widgetIndex) {
    if (!trayItem || !trayMenu || !anchorItem)
      return false;

    // Close any previously opened menu first
    closeContextMenu(screen);

    trayMenu.trayItem = trayItem;
    trayMenu.widgetSection = widgetSection;
    trayMenu.widgetIndex = widgetIndex;

    var popupMenuWindow = getPopupMenuWindow(screen);
    if (popupMenuWindow) {
      popupMenuWindow.open();
      trayMenu.showAt(anchorItem, menuX, menuY);
    } else {
      return false;
    }
    return true;
  }

  // Close tray menu
  function closeTrayMenu(screen) {
    var popupMenuWindow = getPopupMenuWindow(screen);
    if (popupMenuWindow) {
      // This closes both the window and calls hideMenu on the tray menu
      popupMenuWindow.close();
    }
  }

  // Find a fallback screen, prioritizing 0x0 position (primary)
  function findFallbackScreen() {
    let primaryCandidate = null;
    let firstScreen = null;

    for (let i = 0; i < Quickshell.screens.length; i++) {
      const s = Quickshell.screens[i];
      if (s.x === 0 && s.y === 0) {
        primaryCandidate = s;
      }
      if (!firstScreen) {
        firstScreen = s;
      }
    }

    return primaryCandidate || firstScreen || null;
  }

  // Returns a panel (loads it on-demand if not yet loaded)
  // By default, if panel not found on screen, tries other screens (favoring 0x0)
  // Pass fallback=false to disable this behavior
  function getPanel(name, screen, fallback = true) {
    if (!screen) {
      Logger.d("PanelService", "missing screen for getPanel:", name);
      // If no screen specified, return the first matching panel
      for (var key in registeredPanels) {
        if (key.startsWith(name + "-")) {
          return registeredPanels[key];
        }
      }
      return null;
    }

    var panelKey = `${name}-${screen.name}`;

    // Check if panel is already loaded
    if (registeredPanels[panelKey]) {
      return registeredPanels[panelKey];
    }

    // If fallback enabled, try to find panel on another screen
    if (fallback) {
      // First try the primary screen (0x0)
      var fallbackScreen = findFallbackScreen();
      if (fallbackScreen && fallbackScreen.name !== screen.name) {
        var fallbackKey = `${name}-${fallbackScreen.name}`;
        if (registeredPanels[fallbackKey]) {
          Logger.d("PanelService", "Panel fallback from", screen.name, "to", fallbackScreen.name);
          return registeredPanels[fallbackKey];
        }
      }

      // Try any other screen
      for (var key in registeredPanels) {
        if (key.startsWith(name + "-")) {
          Logger.d("PanelService", "Panel fallback to first available:", key);
          return registeredPanels[key];
        }
      }
    }

    Logger.w("PanelService", "Panel not found:", panelKey);
    return null;
  }

  // Check if a panel exists
  function hasPanel(name) {
    return name in registeredPanels;
  }

  // Check if panels can be shown on a given screen (has bar enabled or allowPanelsOnScreenWithoutBar)
  function canShowPanelsOnScreen(screen) {
    const name = screen?.name || "";
    const monitors = Settings.data.bar.monitors || [];
    const allowPanelsOnScreenWithoutBar = Settings.data.general.allowPanelsOnScreenWithoutBar;
    return allowPanelsOnScreenWithoutBar || monitors.length === 0 || monitors.includes(name);
  }

  // Find a screen that can show panels
  function findScreenForPanels() {
    for (let i = 0; i < Quickshell.screens.length; i++) {
      if (canShowPanelsOnScreen(Quickshell.screens[i])) {
        return Quickshell.screens[i];
      }
    }
    return null;
  }

  // Timer to switch from Exclusive to OnDemand keyboard focus on Hyprland
  Timer {
    id: keyboardInitTimer
    interval: 100
    repeat: false
    onTriggered: {
      root.isInitializingKeyboard = false;
    }
  }

  // Remember the toplevel that currently holds focus, before any panel grabs the
  // keyboard, so it can be refocused when panels fully close (Hyprland workaround).
  function captureFocusForRestore() {
    windowIdToRestoreFocus = "";
    if (!CompositorService.isHyprland)
      return;
    var w = CompositorService.getFocusedWindow();
    if (w && w.id)
      windowIdToRestoreFocus = String(w.id);
  }

  // Hand keyboard focus back to the window that was focused before the panel opened.
  // Only acts on Hyprland when follow_mouse != 1 (otherwise the compositor restores
  // focus on its own, and forcing it would fight focus-follows-mouse). No-op elsewhere.
  // (focusWindow also re-raises the window via alterzorder, which is fine: it was the
  //  active/top window before the panel opened.)
  function restoreFocusAfterClose() {
    var id = windowIdToRestoreFocus;
    windowIdToRestoreFocus = "";
    if (!id)
      return;
    if (!CompositorService.isHyprland)
      return;
    if (CompositorService.hyprlandFollowMouse === 1)
      return;
    Logger.d("PanelService", "Restoring keyboard focus to window:", id);
    CompositorService.focusWindow({
                                    "id": id
                                  });
  }

  // Helper to keep only one panel open at any time
  function willOpenPanel(panel) {
    // Capture pre-panel focus only when no panel is currently open (the first panel
    // in a sequence); switching between panels keeps the originally focused window.
    if (openedPanel === null) {
      captureFocusForRestore();
    }

    // Close overlay launcher if open
    if (overlayLauncherOpen) {
      overlayLauncherOpen = false;
      overlayLauncherScreen = null;
    }

    if (openedPanel && openedPanel !== panel) {
      // Move current panel to closing slot before closing it
      closingPanel = openedPanel;
      assignToSlot(1, closingPanel);
      openedPanel.close();
    }

    // Assign new panel to open slot
    openedPanel = panel;
    assignToSlot(0, panel);

    // Start keyboard initialization period (for Hyprland workaround)
    if (panel && panel.exclusiveKeyboard) {
      isInitializingKeyboard = true;
      keyboardInitTimer.restart();
    }

    // emit signal
    willOpen();
  }

  // Open launcher panel (handles both normal and overlay mode)
  function openLauncher(screen) {
    if (Settings.data.appLauncher.overviewLayer) {
      // Close any regular panel first
      if (openedPanel) {
        closingPanel = openedPanel;
        assignToSlot(1, closingPanel);
        openedPanel.close();
        openedPanel = null;
      }
      // Open overlay launcher
      overlayLauncherOpen = true;
      overlayLauncherScreen = screen;
      willOpen();
    } else {
      // Normal mode - use the SmartPanel
      var panel = getPanel("launcherPanel", screen);
      if (panel)
        panel.open();
    }
  }

  // Toggle launcher panel
  function toggleLauncher(screen) {
    if (Settings.data.appLauncher.overviewLayer) {
      if (overlayLauncherOpen && overlayLauncherScreen === screen) {
        closeOverlayLauncher();
      } else {
        openLauncher(screen);
      }
    } else {
      var panel = getPanel("launcherPanel", screen);
      if (panel)
        panel.toggle();
    }
  }

  // Close overlay launcher
  function closeOverlayLauncher() {
    if (overlayLauncherOpen) {
      overlayLauncherOpen = false;
      overlayLauncherScreen = null;
      didClose();
    }
  }

  // Close overlay launcher immediately (for app launches)
  function closeOverlayLauncherImmediately() {
    if (overlayLauncherOpen) {
      closedImmediately = true;
      overlayLauncherOpen = false;
      overlayLauncherScreen = null;
      didClose();
    }
  }

  // ==================== Unified Launcher API ====================
  // These methods work for both normal (SmartPanel) and overlay modes

  function isLauncherOpen(screen) {
    if (Settings.data.appLauncher.overviewLayer) {
      return overlayLauncherOpen && overlayLauncherScreen === screen;
    } else {
      var panel = getPanel("launcherPanel", screen);
      return panel ? panel.isPanelOpen : false;
    }
  }

  function getLauncherSearchText(screen) {
    if (Settings.data.appLauncher.overviewLayer) {
      return overlayLauncherCore ? overlayLauncherCore.searchText : "";
    } else {
      var panel = getPanel("launcherPanel", screen);
      return panel ? panel.searchText : "";
    }
  }

  function setLauncherSearchText(screen, text) {
    if (Settings.data.appLauncher.overviewLayer) {
      if (overlayLauncherCore)
        overlayLauncherCore.setSearchText(text);
    } else {
      var panel = getPanel("launcherPanel", screen);
      if (panel)
        panel.setSearchText(text);
    }
  }

  function openLauncherWithSearch(screen, searchText) {
    if (Settings.data.appLauncher.overviewLayer) {
      openLauncher(screen);
      // Set search text after core is ready
      Qt.callLater(() => {
                     if (overlayLauncherCore)
                     overlayLauncherCore.setSearchText(searchText);
                   });
    } else {
      var panel = getPanel("launcherPanel", screen);
      if (panel) {
        panel.open();
        panel.setSearchText(searchText);
      }
    }
  }

  function closeLauncher(screen) {
    if (Settings.data.appLauncher.overviewLayer) {
      closeOverlayLauncher();
    } else {
      var panel = getPanel("launcherPanel", screen);
      if (panel)
        panel.close();
    }
  }

  // Close any open panel (for general use)
  function closePanel() {
    if (overlayLauncherOpen) {
      closeOverlayLauncher();
    } else if (openedPanel && openedPanel.close) {
      openedPanel.close();
    }
  }

  function closedPanel(panel) {
    var wasActivePanel = (openedPanel && openedPanel === panel);
    if (wasActivePanel) {
      openedPanel = null;
      assignToSlot(0, null);
    }

    if (closingPanel && closingPanel === panel) {
      closingPanel = null;
      assignToSlot(1, null);
    }

    // Reset keyboard init state
    isInitializingKeyboard = false;
    keyboardInitTimer.stop();

    // When the active panel fully closes via a normal (animated) close, nudge
    // Hyprland to refocus the window that was focused before it opened. Skipped for
    // immediate closes (app launches) where the launched app should take focus.
    if (wasActivePanel && !closedImmediately) {
      restoreFocusAfterClose();
    }

    // emit signal
    didClose();
  }

  // Close panels when compositor overview opens (if setting is enabled)
  Connections {
    target: CompositorService
    enabled: Settings.data.bar.hideOnOverview

    function onOverviewActiveChanged() {
      if (CompositorService.overviewActive && root.openedPanel) {
        root.openedPanel.close();
      }
    }
  }
}
