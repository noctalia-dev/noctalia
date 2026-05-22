import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import qs.Commons
import qs.Services.Compositor
import qs.Widgets

ColumnLayout {
  id: root
  spacing: Style.marginL
  Layout.fillWidth: true

  readonly property string effectiveWheelAction: Settings.data.bar.mouseWheelAction || "none"
  readonly property string effectiveMiddleClickAction: Settings.data.bar.middleClickAction || "none"
  readonly property string effectiveRightClickAction: Settings.data.bar.rightClickAction || "controlCenter"

  NComboBox {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-workspace-scroll-label")
    description: I18n.tr("panels.bar.behavior-workspace-scroll-description")
    model: {
      var items = [
            {
              "key": "none",
              "name": I18n.tr("common.none")
            },
            {
              "key": "volume",
              "name": I18n.tr("common.volume")
            },
            {
              "key": "workspace",
              "name": I18n.tr("panels.bar.behavior-workspace-scroll-option-workspace")
            }
          ];
      if (CompositorService.supportsWorkspaceContentScroll) {
        items.push({
                     "key": "content",
                     "name": I18n.tr("panels.bar.behavior-workspace-scroll-option-content")
                   });
      }
      return items;
    }
    currentKey: root.effectiveWheelAction
    defaultValue: Settings.getDefaultValue("bar.mouseWheelAction")
    onSelected: key => Settings.data.bar.mouseWheelAction = key
  }

  NToggle {
    Layout.fillWidth: true
    label: I18n.tr("panels.general.reverse-scrolling-label")
    description: I18n.tr("panels.general.reverse-scrolling-description")
    checked: Settings.data.bar.reverseScroll
    defaultValue: Settings.getDefaultValue("bar.reverseScroll")
    onToggled: checked => Settings.data.bar.reverseScroll = checked
    visible: Settings.data.bar.mouseWheelAction !== "none"
  }

  NToggle {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-wheel-wrap-label")
    description: I18n.tr("panels.bar.behavior-wheel-wrap-description")
    checked: Settings.data.bar.mouseWheelWrap
    defaultValue: Settings.getDefaultValue("bar.mouseWheelWrap")
    onToggled: checked => Settings.data.bar.mouseWheelWrap = checked
    visible: Settings.data.bar.mouseWheelAction === "workspace"
  }

  NComboBox {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-middle-click-label")
    description: I18n.tr("panels.bar.behavior-middle-click-description")
    model: [
      {
        "key": "none",
        "name": I18n.tr("common.none")
      },
      {
        "key": "controlCenter",
        "name": I18n.tr("tooltips.open-control-center")
      },
      {
        "key": "settings",
        "name": I18n.tr("tooltips.open-settings")
      },
      {
        "key": "launcherPanel",
        "name": I18n.tr("actions.open-launcher")
      },
      {
        "key": "command",
        "name": I18n.tr("actions.run-custom-command")
      }
    ]
    currentKey: root.effectiveMiddleClickAction
    defaultValue: Settings.getDefaultValue("bar.middleClickAction")
    onSelected: key => Settings.data.bar.middleClickAction = key
  }

  NTextInput {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-middle-click-command-label")
    description: I18n.tr("panels.bar.behavior-middle-click-command-description")
    placeholderText: I18n.tr("panels.bar.behavior-middle-click-command-placeholder")
    text: Settings.data.bar.middleClickCommand
    fontFamily: Settings.data.ui.fontFixed
    onTextChanged: Settings.data.bar.middleClickCommand = text
    visible: Settings.data.bar.middleClickAction === "command"
  }

  NToggle {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-middle-click-follow-mouse-label")
    description: I18n.tr("panels.bar.behavior-middle-click-follow-mouse-description")
    checked: Settings.data.bar.middleClickFollowMouse
    defaultValue: Settings.getDefaultValue("bar.middleClickFollowMouse")
    onToggled: checked => Settings.data.bar.middleClickFollowMouse = checked
    visible: Settings.data.bar.middleClickAction !== "none" && Settings.data.bar.middleClickAction !== "command" && !(Settings.data.bar.middleClickAction === "settings" && Settings.data.ui.settingsPanelMode === "window")
  }

  NComboBox {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-right-click-label")
    description: I18n.tr("panels.bar.behavior-right-click-description")
    model: [
      {
        "key": "none",
        "name": I18n.tr("common.none")
      },
      {
        "key": "controlCenter",
        "name": I18n.tr("tooltips.open-control-center")
      },
      {
        "key": "settings",
        "name": I18n.tr("tooltips.open-settings")
      },
      {
        "key": "launcherPanel",
        "name": I18n.tr("actions.open-launcher")
      },
      {
        "key": "command",
        "name": I18n.tr("actions.run-custom-command")
      }
    ]
    currentKey: root.effectiveRightClickAction
    defaultValue: Settings.getDefaultValue("bar.rightClickAction")
    onSelected: key => Settings.data.bar.rightClickAction = key
  }

  NTextInput {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-right-click-command-label")
    description: I18n.tr("panels.bar.behavior-right-click-command-description")
    placeholderText: I18n.tr("panels.bar.behavior-right-click-command-placeholder")
    text: Settings.data.bar.rightClickCommand
    fontFamily: Settings.data.ui.fontFixed
    onTextChanged: Settings.data.bar.rightClickCommand = text
    visible: Settings.data.bar.rightClickAction === "command"
  }

  NToggle {
    Layout.fillWidth: true
    label: I18n.tr("panels.bar.behavior-right-click-follow-mouse-label")
    description: I18n.tr("panels.bar.behavior-right-click-follow-mouse-description")
    checked: Settings.data.bar.rightClickFollowMouse
    defaultValue: Settings.getDefaultValue("bar.rightClickFollowMouse")
    onToggled: checked => Settings.data.bar.rightClickFollowMouse = checked
    visible: Settings.data.bar.rightClickAction !== "none" && Settings.data.bar.rightClickAction !== "command" && !(Settings.data.bar.rightClickAction === "settings" && Settings.data.ui.settingsPanelMode === "window")
  }
}
