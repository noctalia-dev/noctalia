pragma Singleton

import QtQuick
import Quickshell
import qs.Commons
import qs.Modules.Bar.Widgets

Singleton {
  id: root

  signal pluginWidgetRegistryUpdated

  // Widget registry object mapping widget names to components
  property var widgets: ({
                           "ActiveWindow": activeWindowComponent,
                           "AudioVisualizer": audioVisualizerComponent,
                           "Battery": batteryComponent,
                           "Bluetooth": bluetoothComponent,
                           "Brightness": brightnessComponent,
                           "Clock": clockComponent,
                           "ControlCenter": controlCenterComponent,
                           "CustomButton": customButtonComponent,
                           "DarkMode": darkModeComponent,
                           "KeepAwake": keepAwakeComponent,
                           "KeyboardLayout": keyboardLayoutComponent,
                           "LockKeys": lockKeysComponent,
                           "Launcher": launcherComponent,
                           "MediaMini": mediaMiniComponent,
                           "Microphone": microphoneComponent,
                           "Network": networkComponent,
                           "NightLight": nightLightComponent,
                           "NoctaliaPerformance": noctaliaPerformanceComponent,
                           "NotificationHistory": notificationHistoryComponent,
                           "PowerProfile": powerProfileComponent,
                           "SessionMenu": sessionMenuComponent,
                           "Settings": settingsComponent,
                           "Spacer": spacerComponent,
                           "SystemMonitor": systemMonitorComponent,
                           "Taskbar": taskbarComponent,
                           "Tray": trayComponent,
                           "Volume": volumeComponent,
                           "VPN": vpnComponent,
                           "WallpaperSelector": wallpaperSelectorComponent,
                           "Workspace": workspaceComponent
                         })

  property var widgetSettingsMap: ({
                                     "ActiveWindow": "WidgetSettings/ActiveWindowSettings.qml",
                                     "AudioVisualizer": "WidgetSettings/AudioVisualizerSettings.qml",
                                     "Battery": "WidgetSettings/BatterySettings.qml",
                                     "Bluetooth": "WidgetSettings/BluetoothSettings.qml",
                                     "Brightness": "WidgetSettings/BrightnessSettings.qml",
                                     "Clock": "WidgetSettings/ClockSettings.qml",
                                     "ControlCenter": "WidgetSettings/ControlCenterSettings.qml",
                                     "CustomButton": "WidgetSettings/CustomButtonSettings.qml",
                                     "DarkMode": "WidgetSettings/DarkModeSettings.qml",
                                     "KeepAwake": "WidgetSettings/KeepAwakeSettings.qml",
                                     "KeyboardLayout": "WidgetSettings/KeyboardLayoutSettings.qml",
                                     "Launcher": "WidgetSettings/LauncherSettings.qml",
                                     "LockKeys": "WidgetSettings/LockKeysSettings.qml",
                                     "MediaMini": "WidgetSettings/MediaMiniSettings.qml",
                                     "Microphone": "WidgetSettings/MicrophoneSettings.qml",
                                     "Network": "WidgetSettings/NetworkSettings.qml",
                                     "NightLight": "WidgetSettings/NightLightSettings.qml",
                                     "NoctaliaPerformance": "WidgetSettings/NoctaliaPerformanceSettings.qml",
                                     "NotificationHistory": "WidgetSettings/NotificationHistorySettings.qml",
                                     "PowerProfile": "WidgetSettings/PowerProfileSettings.qml",
                                     "SessionMenu": "WidgetSettings/SessionMenuSettings.qml",
                                     "Settings": "WidgetSettings/SettingsSettings.qml",
                                     "Spacer": "WidgetSettings/SpacerSettings.qml",
                                     "SystemMonitor": "WidgetSettings/SystemMonitorSettings.qml",
                                     "Taskbar": "WidgetSettings/TaskbarSettings.qml",
                                     "Tray": "WidgetSettings/TraySettings.qml",
                                     "Volume": "WidgetSettings/VolumeSettings.qml",
                                     "VPN": "WidgetSettings/VPNSettings.qml",
                                     "WallpaperSelector": "WidgetSettings/WallpaperSelectorSettings.qml",
                                     "Workspace": "WidgetSettings/WorkspaceSettings.qml"
                                   })

  property var widgetMetadata: ({
                                  "ActiveWindow": {
                                    "showText": true,
                                    "showIcon": true,
                                    "hideMode": "hidden",
                                    "scrollingMode": "hover",
                                    "maxWidth": 145,
                                    "useFixedWidth": false,
                                    "colorizeIcons": false,
                                    "textColor": "none"
                                  },
                                  "AudioVisualizer": {
                                    "width": 200,
                                    "colorName": "primary",
                                    "hideWhenIdle": false
                                  },
                                  "Battery": {
                                    "displayMode": "graphic-clean",
                                    "deviceNativePath": "__default__",
                                    "showPowerProfiles": false,
                                    "showNoctaliaPerformance": false,
                                    "hideIfNotDetected": true,
                                    "hideIfIdle": false
                                  },
                                  "Bluetooth": {
                                    "displayMode": "onhover",
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "Brightness": {
                                    "displayMode": "onhover",
                                    "iconColor": "none",
                                    "textColor": "none",
                                    "applyToAllMonitors": false
                                  },
                                  "Clock": {
                                    "clockColor": "none",
                                    "useCustomFont": false,
                                    "customFont": "",
                                    "formatHorizontal": "HH:mm ddd, MMM dd",
                                    "formatVertical": "HH mm - dd MM",
                                    "tooltipFormat": "HH:mm ddd, MMM dd"
                                  },
                                  "ControlCenter": {
                                    "useDistroLogo": false,
                                    "icon": "noctalia",
                                    "customIconPath": "",
                                    "colorizeDistroLogo": false,
                                    "colorizeSystemIcon": "none",
                                    "colorizeSystemText": "none",
                                    "enableColorization": false
                                  },
                                  "CustomButton": {
                                    "icon": "heart",
                                    "showIcon": true,
                                    "iconPosition": "left",
                                    "showExecTooltip": true,
                                    "showTextTooltip": true,
                                    "generalTooltipText": "",
                                    "hideMode": "alwaysExpanded",
                                    "leftClickExec": "",
                                    "leftClickUpdateText": false,
                                    "rightClickExec": "",
                                    "rightClickUpdateText": false,
                                    "middleClickExec": "",
                                    "middleClickUpdateText": false,
                                    "textCommand": "",
                                    "textStream": false,
                                    "textIntervalMs": 3000,
                                    "textCollapse": "",
                                    "parseJson": false,
                                    "wheelExec": "",
                                    "wheelUpExec": "",
                                    "wheelDownExec": "",
                                    "wheelMode": "unified",
                                    "wheelUpdateText": false,
                                    "wheelUpUpdateText": false,
                                    "wheelDownUpdateText": false,
                                    "maxTextLength": {
                                      "horizontal": 10,
                                      "vertical": 10
                                    },
                                    "colorizeSystemIcon": "none",
                                    "colorizeSystemText": "none",
                                    "ipcIdentifier": ""
                                  },
                                  "DarkMode": {
                                    "iconColor": "none"
                                  },
                                  "KeepAwake": {
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "KeyboardLayout": {
                                    "displayMode": "onhover",
                                    "showIcon": true,
                                    "iconColor": "none",
                                    "textColor": "none",
                                    "customLabels": {}
                                  },
                                  "LockKeys": {
                                    "showCapsLock": true,
                                    "showNumLock": true,
                                    "showScrollLock": true,
                                    "capsLockIcon": "letter-c",
                                    "numLockIcon": "letter-n",
                                    "scrollLockIcon": "letter-s",
                                    "hideWhenOff": false
                                  },
                                  "Launcher": {
                                    "useDistroLogo": false,
                                    "icon": "rocket",
                                    "customIconPath": "",
                                    "colorizeSystemIcon": "none",
                                    "colorizeSystemText": "none",
                                    "enableColorization": false,
                                    "iconColor": "none"
                                  },
                                  "MediaMini": {
                                    "hideMode": "hidden",
                                    "scrollingMode": "hover",
                                    "maxWidth": 145,
                                    "useFixedWidth": false,
                                    "hideWhenIdle": false,
                                    "showAlbumArt": true,
                                    "showArtistFirst": true,
                                    "showVisualizer": false,
                                    "showProgressRing": true,
                                    "visualizerType": "linear",
                                    "textColor": "none",
                                    "compactMode": false,
                                    "panelShowAlbumArt": true
                                  },
                                  "Microphone": {
                                    "displayMode": "onhover",
                                    "middleClickCommand": "pwvucontrol || pavucontrol",
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "NotificationHistory": {
                                    "showUnreadBadge": true,
                                    "hideWhenZero": false,
                                    "hideWhenZeroUnread": false,
                                    "unreadBadgeColor": "primary",
                                    "iconColor": "none"
                                  },
                                  "SessionMenu": {
                                    "iconColor": "error"
                                  },
                                  "Settings": {
                                    "iconColor": "none"
                                  },
                                  "Spacer": {
                                    "width": 20
                                  },
                                  "SystemMonitor": {
                                    "compactMode": true,
                                    "iconColor": "none",
                                    "textColor": "none",
                                    "useMonospaceFont": true,
                                    "usePadding": false,
                                    "showCpuUsage": true,
                                    "showCpuCores": false,
                                    "showCpuFreq": false,
                                    "showCpuTemp": true,
                                    "showGpuTemp": false,
                                    "showLoadAverage": false,
                                    "showMemoryUsage": true,
                                    "showMemoryAsPercent": false,
                                    "showSwapUsage": false,
                                    "showNetworkStats": false,
                                    "showDiskUsage": false,
                                    "showDiskUsageAsPercent": false,
                                    "showDiskAvailable": false,
                                    "diskPath": "/"
                                  },
                                  "Taskbar": {
                                    "onlySameOutput": true,
                                    "onlyActiveWorkspaces": true,
                                    "hideMode": "hidden",
                                    "colorizeIcons": false,
                                    "showTitle": false,
                                    "titleWidth": 120,
                                    "showPinnedApps": true,
                                    "smartWidth": true,
                                    "maxTaskbarWidth": 40,
                                    "iconScale": 0.8
                                  },
                                  "Tray": {
                                    "blacklist": [],
                                    "colorizeIcons": false,
                                    "chevronColor": "none",
                                    "pinned": [],
                                    "drawerEnabled": true,
                                    "hidePassive": false
                                  },
                                  "VPN": {
                                    "displayMode": "onhover",
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "Network": {
                                    "displayMode": "onhover",
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "NightLight": {
                                    "iconColor": "none"
                                  },
                                  "NoctaliaPerformance": {
                                    "iconColor": "none"
                                  },
                                  "PowerProfile": {
                                    "iconColor": "none"
                                  },
                                  "Workspace": {
                                    "labelMode": "index",
                                    "followFocusedScreen": false,
                                    "hideUnoccupied": false,
                                    "characterCount": 2,
                                    "showApplications": false,
                                    "showApplicationsHover": false,
                                    "showLabelsOnlyWhenOccupied": true,
                                    "colorizeIcons": false,
                                    "unfocusedIconsOpacity": 1.0,
                                    "groupedBorderOpacity": 1.0,
                                    "enableScrollWheel": true,
                                    "iconScale": 0.8,
                                    "focusedColor": "primary",
                                    "occupiedColor": "secondary",
                                    "emptyColor": "secondary",
                                    "showBadge": true,
                                    "pillSize": 0.6,
                                    "fontWeight": "bold"
                                  },
                                  "Volume": {
                                    "displayMode": "onhover",
                                    "middleClickCommand": "pwvucontrol || pavucontrol",
                                    "iconColor": "none",
                                    "textColor": "none"
                                  },
                                  "WallpaperSelector": {
                                    "iconColor": "none"
                                  }
                                })

  // Component definitions - these are loaded once at startup
  property Component activeWindowComponent: Component {
    ActiveWindow {}
  }
  property Component audioVisualizerComponent: Component {
    AudioVisualizer {}
  }
  property Component batteryComponent: Component {
    Battery {}
  }
  property Component bluetoothComponent: Component {
    Bluetooth {}
  }
  property Component brightnessComponent: Component {
    Brightness {}
  }
  property Component clockComponent: Component {
    Clock {}
  }
  property Component customButtonComponent: Component {
    CustomButton {}
  }
  property Component darkModeComponent: Component {
    DarkMode {}
  }
  property Component keyboardLayoutComponent: Component {
    KeyboardLayout {}
  }
  property Component keepAwakeComponent: Component {
    KeepAwake {}
  }
  property Component lockKeysComponent: Component {
    LockKeys {}
  }
  property Component launcherComponent: Component {
    Launcher {}
  }
  property Component mediaMiniComponent: Component {
    MediaMini {}
  }
  property Component microphoneComponent: Component {
    Microphone {}
  }
  property Component nightLightComponent: Component {
    NightLight {}
  }
  property Component noctaliaPerformanceComponent: Component {
    NoctaliaPerformance {}
  }
  property Component notificationHistoryComponent: Component {
    NotificationHistory {}
  }
  property Component powerProfileComponent: Component {
    PowerProfile {}
  }
  property Component sessionMenuComponent: Component {
    SessionMenu {}
  }
  property Component settingsComponent: Component {
    Settings {}
  }
  property Component controlCenterComponent: Component {
    ControlCenter {}
  }
  property Component spacerComponent: Component {
    Spacer {}
  }
  property Component systemMonitorComponent: Component {
    SystemMonitor {}
  }
  property Component trayComponent: Component {
    Tray {}
  }
  property Component volumeComponent: Component {
    Volume {}
  }
  property Component vpnComponent: Component {
    VPN {}
  }
  property Component networkComponent: Component {
    Network {}
  }
  property Component wallpaperSelectorComponent: Component {
    WallpaperSelector {}
  }
  property Component workspaceComponent: Component {
    Workspace {}
  }
  property Component taskbarComponent: Component {
    Taskbar {}
  }
  function init() {
    Logger.i("BarWidgetRegistry", "Service started");
  }

  // ------------------------------
  // Helper function to get widget component by name
  function getWidget(id) {
    return widgets[id] || null;
  }

  // Helper function to check if widget exists
  function hasWidget(id) {
    return id in widgets;
  }

  // Get list of available widget id
  function getAvailableWidgets() {
    return Object.keys(widgets);
  }

  // Helper function to check if widget has user settings
  function widgetHasUserSettings(id) {
    return widgetMetadata[id] !== undefined;
  }

  // ------------------------------
  // Plugin widget registration

  // Track plugin widgets separately
  property var pluginWidgets: ({})
  property var pluginWidgetMetadata: ({})

  // Register a plugin widget
  function registerPluginWidget(pluginId, component, metadata) {
    if (!pluginId || !component) {
      Logger.e("BarWidgetRegistry", "Cannot register plugin widget: invalid parameters");
      return false;
    }

    // Add plugin: prefix to avoid conflicts with core widgets
    var widgetId = "plugin:" + pluginId;

    pluginWidgets[widgetId] = component;
    pluginWidgetMetadata[widgetId] = metadata || {};

    // Also add to main widgets object for unified access
    widgets[widgetId] = component;
    widgetMetadata[widgetId] = metadata || {};

    Logger.i("BarWidgetRegistry", "Registered plugin widget:", widgetId);
    root.pluginWidgetRegistryUpdated();
    return true;
  }

  // Unregister a plugin widget
  function unregisterPluginWidget(pluginId) {
    var widgetId = "plugin:" + pluginId;

    if (!pluginWidgets[widgetId]) {
      Logger.w("BarWidgetRegistry", "Plugin widget not registered:", widgetId);
      return false;
    }

    delete pluginWidgets[widgetId];
    delete pluginWidgetMetadata[widgetId];
    delete widgets[widgetId];
    delete widgetMetadata[widgetId];

    Logger.i("BarWidgetRegistry", "Unregistered plugin widget:", widgetId);
    root.pluginWidgetRegistryUpdated();
    return true;
  }

  // Check if a widget is a plugin widget
  function isPluginWidget(id) {
    return id.startsWith("plugin:");
  }

  property var cpuIntensiveWidgets: ["AudioVisualizer"]

  function isCpuIntensive(id) {
    if (pluginWidgetMetadata[id]?.cpuIntensive)
      return true;
    return cpuIntensiveWidgets.indexOf(id) >= 0;
  }

  // Get list of plugin widget IDs
  function getPluginWidgets() {
    return Object.keys(pluginWidgets);
  }
}
