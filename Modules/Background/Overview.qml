import QtQuick
import QtQuick.Effects
import Quickshell
import Quickshell.Wayland
import qs.Commons
import qs.Services.Compositor
import qs.Services.Power
import qs.Services.UI

Loader {
  active: CompositorService.supportsOverview && Settings.data.wallpaper.enabled && Settings.data.wallpaper.overviewEnabled && (!PowerProfileService.noctaliaPerformanceMode || !Settings.data.noctaliaPerformance.disableWallpaper)

  sourceComponent: Variants {
    model: Quickshell.screens

    delegate: PanelWindow {
      id: panelWindow

      required property ShellScreen modelData
      property string wallpaper: ""
      property string preprocessedWallpaper: "" // Pre-resized wallpaper from Background.qml
      property bool isSolidColor: Settings.data.wallpaper.useSolidColor
      property color solidColor: Settings.data.wallpaper.solidColor
      property color tintColor: Settings.data.colorSchemes.darkMode ? Color.mSurface : Color.mOnSurface

      visible: wallpaper !== "" || isSolidColor

      Component.onCompleted: {
        if (modelData) {
          Logger.d("Overview", "Loading overview on", modelData.name);
        }
      }

      Component.onDestruction: {
        bgImage.source = "";
      }

      // External state management - wait for wallpaper processing to complete
      // Reuses the same cached image as Background.qml for memory efficiency + GPU blur
      Connections {
        target: WallpaperService
        function onWallpaperProcessingComplete(screenName, path, cachedPath) {
          if (screenName === modelData.name) {
            preprocessedWallpaper = cachedPath || "";
            wallpaper = path;
          }
        }
      }

      // Handle wallpaper changes (solid color detection)
      onWallpaperChanged: {
        if (!wallpaper)
          return;

        // Check if this is a solid color path
        if (WallpaperService.isSolidColorPath(wallpaper)) {
          isSolidColor = true;
          var colorStr = WallpaperService.getSolidColor(wallpaper);
          solidColor = colorStr;
          return;
        }

        isSolidColor = false;
      }

      color: "transparent"
      screen: modelData
      WlrLayershell.layer: WlrLayer.Background
      WlrLayershell.exclusionMode: ExclusionMode.Ignore
      WlrLayershell.namespace: "noctalia-overview-" + (screen?.name || "unknown")

      anchors {
        top: true
        bottom: true
        right: true
        left: true
      }

      // Solid color background
      Rectangle {
        anchors.fill: parent
        visible: isSolidColor
        color: solidColor

        Rectangle {
          anchors.fill: parent
          color: tintColor
          opacity: Settings.data.wallpaper.overviewTint
        }
      }

      // Image background with GPU-based blur
      Image {
        id: bgImage
        anchors.fill: parent
        visible: !isSolidColor
        fillMode: Image.PreserveAspectCrop
        source: preprocessedWallpaper || wallpaper
        smooth: true
        mipmap: false
        cache: true // Shares texture with Background's currentWallpaper
        asynchronous: true

        layer.enabled: Settings.data.wallpaper.overviewBlur > 0 && !PowerProfileService.noctaliaPerformanceMode
        layer.smooth: false
        layer.effect: MultiEffect {
          blurEnabled: true
          blur: Settings.data.wallpaper.overviewBlur
          blurMax: 48
        }

        // Tint overlay
        Rectangle {
          anchors.fill: parent
          color: tintColor
          opacity: Settings.data.wallpaper.overviewTint
        }
      }
    }
  }
}
