import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import qs.Commons
import qs.Services.Compositor
import qs.Services.UI
import qs.Widgets

ColumnLayout {
  id: root
  spacing: Style.marginL
  Layout.fillWidth: true

  property var screen

  signal openMainFolderPicker
  signal openMonitorFolderPicker(string monitorName)

  NToggle {
    label: I18n.tr("panels.wallpaper.settings-enable-management-label")
    description: I18n.tr("panels.wallpaper.settings-enable-management-description")
    checked: Settings.data.wallpaper.enabled
    onToggled: checked => Settings.data.wallpaper.enabled = checked
    defaultValue: Settings.getDefaultValue("wallpaper.enabled")
  }

  ColumnLayout {
    enabled: Settings.data.wallpaper.enabled
    spacing: Style.marginL
    Layout.fillWidth: true

    RowLayout {

      NLabel {
        label: I18n.tr("tooltips.wallpaper-selector")
        description: I18n.tr("panels.wallpaper.settings-selector-description")
        Layout.alignment: Qt.AlignTop
      }

      NIconButton {
        icon: "wallpaper-selector"
        tooltipText: I18n.tr("tooltips.wallpaper-selector")
        onClicked: PanelService.getPanel("wallpaperPanel", root.screen)?.toggle()
      }
    }

    NComboBox {
      label: I18n.tr("common.position")
      description: I18n.tr("panels.wallpaper.settings-selector-position-description")
      Layout.fillWidth: true
      model: [
        {
          "key": "follow_bar",
          "name": I18n.tr("positions.follow-bar")
        },
        {
          "key": "center",
          "name": I18n.tr("positions.center")
        },
        {
          "key": "top_center",
          "name": I18n.tr("positions.top-center")
        },
        {
          "key": "top_left",
          "name": I18n.tr("positions.top-left")
        },
        {
          "key": "top_right",
          "name": I18n.tr("positions.top-right")
        },
        {
          "key": "bottom_left",
          "name": I18n.tr("positions.bottom-left")
        },
        {
          "key": "bottom_right",
          "name": I18n.tr("positions.bottom-right")
        },
        {
          "key": "bottom_center",
          "name": I18n.tr("positions.bottom-center")
        }
      ]
      currentKey: Settings.data.wallpaper.panelPosition
      onSelected: key => Settings.data.wallpaper.panelPosition = key
      defaultValue: Settings.getDefaultValue("wallpaper.panelPosition")
    }

    NComboBox {
      label: I18n.tr("panels.wallpaper.settings-view-mode-label")
      description: I18n.tr("panels.wallpaper.settings-view-mode-description")
      Layout.fillWidth: true
      model: [
        {
          "key": "single",
          "name": I18n.tr("panels.wallpaper.view-mode-single")
        },
        {
          "key": "recursive",
          "name": I18n.tr("panels.wallpaper.view-mode-recursive")
        },
        {
          "key": "browse",
          "name": I18n.tr("panels.wallpaper.view-mode-browse")
        }
      ]
      currentKey: Settings.data.wallpaper.viewMode
      onSelected: key => Settings.data.wallpaper.viewMode = key
      defaultValue: Settings.getDefaultValue("wallpaper.viewMode")
    }

    NTextInputButton {
      id: wallpaperPathInput
      label: I18n.tr("panels.wallpaper.settings-folder-label")
      description: I18n.tr("panels.wallpaper.settings-folder-description")
      text: Settings.data.wallpaper.directory
      buttonIcon: "folder-open"
      buttonTooltip: I18n.tr("panels.wallpaper.settings-folder-label")
      Layout.fillWidth: true
      onInputTextChanged: text => Settings.data.wallpaper.directory = text
      onButtonClicked: root.openMainFolderPicker()
    }

    NToggle {
      label: I18n.tr("panels.wallpaper.settings-monitor-specific-label")
      description: I18n.tr("panels.wallpaper.settings-monitor-specific-description")
      checked: Settings.data.wallpaper.enableMultiMonitorDirectories
      onToggled: checked => Settings.data.wallpaper.enableMultiMonitorDirectories = checked
      defaultValue: Settings.getDefaultValue("wallpaper.enableMultiMonitorDirectories")
    }

    NBox {
      visible: Settings.data.wallpaper.enableMultiMonitorDirectories
      Layout.fillWidth: true
      radius: Style.radiusM
      color: Color.mSurface
      border.color: Color.mOutline
      border.width: Style.borderS
      implicitHeight: contentCol.implicitHeight + Style.margin2L
      clip: true

      ColumnLayout {
        id: contentCol
        anchors.fill: parent
        anchors.margins: Style.marginL
        spacing: Style.marginM
        Repeater {
          model: Quickshell.screens || []
          delegate: ColumnLayout {
            Layout.fillWidth: true
            spacing: Style.marginS

            NText {
              text: (modelData.name || "Unknown")
              color: Color.mPrimary
              font.weight: Style.fontWeightBold
              pointSize: Style.fontSizeM
            }

            NTextInputButton {
              id: monitorDirInput
              text: WallpaperService.getMonitorDirectory(modelData.name)
              buttonIcon: "folder-open"
              buttonTooltip: I18n.tr("panels.wallpaper.settings-monitor-specific-tooltip")
              Layout.fillWidth: true
              onInputEditingFinished: WallpaperService.setMonitorDirectory(modelData.name, monitorDirInput.text)
              onButtonClicked: root.openMonitorFolderPicker(modelData.name)
            }
          }
        }
      }
    }
  }

  NDivider {
    Layout.fillWidth: true
  }

  NToggle {
    label: I18n.tr("panels.wallpaper.settings-use-original-images-label")
    description: I18n.tr("panels.wallpaper.settings-use-original-images-description")
    checked: Settings.data.wallpaper.useOriginalImages
    enabled: Settings.data.wallpaper.enabled
    onToggled: checked => Settings.data.wallpaper.useOriginalImages = checked
    defaultValue: Settings.getDefaultValue("wallpaper.useOriginalImages")
  }

  RowLayout {
    spacing: Style.marginM
    Layout.fillWidth: true
    enabled: Settings.data.wallpaper.enabled

    NLabel {
      label: I18n.tr("panels.wallpaper.settings-clear-cache-label")
      description: I18n.tr("panels.wallpaper.settings-clear-cache-description")
      Layout.fillWidth: true
    }

    NButton {
      icon: "trash"
      text: I18n.tr("panels.wallpaper.settings-clear-cache-button")
      outlined: true
      onClicked: {
        ImageCacheService.clearLarge();
        ToastService.showNotice(I18n.tr("panels.wallpaper.settings-clear-cache-toast"));
      }
    }
  }

  NDivider {
    Layout.fillWidth: true
    visible: CompositorService.supportsOverview
  }

  ColumnLayout {
    visible: CompositorService.supportsOverview
    enabled: Settings.data.wallpaper.enabled
    spacing: Style.marginL
    Layout.fillWidth: true

    NToggle {
      label: I18n.tr("panels.wallpaper.settings-enable-overview-label")
      description: I18n.tr("panels.wallpaper.settings-enable-overview-description")
      checked: Settings.data.wallpaper.enabled && Settings.data.wallpaper.overviewEnabled
      onToggled: checked => Settings.data.wallpaper.overviewEnabled = checked
      defaultValue: Settings.getDefaultValue("wallpaper.overviewEnabled")
    }

    NValueSlider {
      Layout.fillWidth: true
      visible: Settings.data.wallpaper.overviewEnabled
      label: I18n.tr("panels.wallpaper.settings-overview-blur-strength-label")
      description: I18n.tr("panels.wallpaper.settings-overview-blur-strength-description")
      from: 0.0
      to: 1.0
      stepSize: 0.01
      showReset: true
      value: Settings.data.wallpaper.overviewBlur
      onMoved: value => Settings.data.wallpaper.overviewBlur = value
      text: ((Settings.data.wallpaper.overviewBlur) * 100).toFixed(0) + "%"
      defaultValue: Settings.getDefaultValue("wallpaper.overviewBlur")
    }

    NValueSlider {
      Layout.fillWidth: true
      visible: Settings.data.wallpaper.overviewEnabled
      label: I18n.tr("panels.wallpaper.settings-overview-tint-label")
      description: I18n.tr("panels.wallpaper.settings-overview-tint-description")
      from: 0.0
      to: 1.0
      stepSize: 0.01
      showReset: true
      value: Settings.data.wallpaper.overviewTint
      onMoved: value => Settings.data.wallpaper.overviewTint = value
      text: ((Settings.data.wallpaper.overviewTint) * 100).toFixed(0) + "%"
      defaultValue: Settings.getDefaultValue("wallpaper.overviewTint")
    }
  }
}
