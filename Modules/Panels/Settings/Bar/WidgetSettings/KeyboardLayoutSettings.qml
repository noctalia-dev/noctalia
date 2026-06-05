import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import qs.Commons
import qs.Services.Keyboard
import qs.Widgets

ColumnLayout {
  id: root
  spacing: Style.marginM

  property var screen: null
  property var widgetData: null
  property var widgetMetadata: null

  signal settingsChanged(var settings)

  property string valueDisplayMode: widgetData.displayMode !== undefined ? widgetData.displayMode : widgetMetadata.displayMode
  property bool valueShowIcon: widgetData.showIcon !== undefined ? widgetData.showIcon : widgetMetadata.showIcon
  property string valueIconColor: widgetData.iconColor !== undefined ? widgetData.iconColor : widgetMetadata.iconColor
  property string valueTextColor: widgetData.textColor !== undefined ? widgetData.textColor : widgetMetadata.textColor
  property var valueCustomLabels: widgetData.customLabels !== undefined ? widgetData.customLabels : (widgetMetadata.customLabels ?? {})

  function saveSettings() {
    var settings = Object.assign({}, widgetData || {});
    settings.displayMode = valueDisplayMode;
    settings.showIcon = valueShowIcon;
    settings.iconColor = valueIconColor;
    settings.textColor = valueTextColor;
    settings.customLabels = valueCustomLabels;
    settingsChanged(settings);
  }

  function saveCustomLabels() {
    var labels = {};
    for (var i = 0; i < customLabelsModel.count; i++) {
      var item = customLabelsModel.get(i);
      if (item.customLabel.trim().length > 0) {
        labels[item.fullName] = item.customLabel.trim();
      }
    }
    valueCustomLabels = labels;
    saveSettings();
  }

  function addCurrentLayout() {
    var current = KeyboardLayoutService.fullLayoutName;
    if (!current || current === "") return;
    for (var i = 0; i < customLabelsModel.count; i++) {
      if (customLabelsModel.get(i).fullName === current) return;
    }
    customLabelsModel.append({ fullName: current, customLabel: "" });
  }

  ListModel {
    id: customLabelsModel

    Component.onCompleted: {
      var labels = valueCustomLabels ?? {};
      var all = KeyboardLayoutService.allLayouts;
      if (all && all.length > 0) {
        for (var i = 0; i < all.length; i++) {
          customLabelsModel.append({ fullName: all[i], customLabel: labels[all[i]] ?? "" });
        }
      } else {
        for (var key in labels) {
          customLabelsModel.append({ fullName: key, customLabel: labels[key] });
        }
      }
    }
  }

  NComboBox {
    visible: valueShowIcon
    label: I18n.tr("common.display-mode")
    description: I18n.tr("bar.volume.display-mode-description")
    minimumWidth: 200
    model: [
      { "key": "onhover",    "name": I18n.tr("display-modes.on-hover") },
      { "key": "forceOpen",  "name": I18n.tr("display-modes.force-open") },
      { "key": "alwaysHide", "name": I18n.tr("display-modes.always-hide") }
    ]
    currentKey: valueDisplayMode
    onSelected: key => {
                  valueDisplayMode = key;
                  saveSettings();
                }
    defaultValue: widgetMetadata.displayMode
  }

  NToggle {
    label: I18n.tr("bar.custom-button.show-icon-label")
    description: I18n.tr("bar.keyboard-layout.show-icon-description")
    checked: valueShowIcon
    onToggled: checked => {
                 valueShowIcon = checked;
                 saveSettings();
               }
    defaultValue: widgetMetadata.showIcon
  }

  NColorChoice {
    label: I18n.tr("common.select-icon-color")
    currentKey: valueIconColor
    onSelected: key => {
                  valueIconColor = key;
                  saveSettings();
                }
    defaultValue: widgetMetadata.iconColor
  }

  NColorChoice {
    currentKey: valueTextColor
    onSelected: key => {
                  valueTextColor = key;
                  saveSettings();
                }
    defaultValue: widgetMetadata.textColor
  }

  NDivider {
    Layout.fillWidth: true
  }

  NLabel {
    label: I18n.tr("bar.keyboard-layout.custom-labels-header")
    description: I18n.tr("bar.keyboard-layout.custom-labels-description")
    Layout.fillWidth: true
  }

  Repeater {
    model: customLabelsModel

    RowLayout {
      spacing: Style.marginS
      Layout.fillWidth: true

      NText {
        text: model.fullName
        Layout.fillWidth: true
        elide: Text.ElideMiddle
        opacity: 0.7
        pointSize: Style.fontSizeS
      }

      NTextInput {
        minimumInputWidth: 80 * Style.uiScaleRatio
        text: model.customLabel
        onTextChanged: {
          customLabelsModel.setProperty(index, "customLabel", text);
          saveCustomLabels();
        }
      }

      NIconButton {
        icon: "trash"
        tooltipText: I18n.tr("bar.keyboard-layout.custom-labels-remove")
        onClicked: {
          customLabelsModel.remove(index);
          saveCustomLabels();
        }
      }
    }
  }

  NButton {
    visible: !KeyboardLayoutService.allLayouts || KeyboardLayoutService.allLayouts.length === 0
    text: I18n.tr("bar.keyboard-layout.custom-labels-add-current")
    onClicked: addCurrentLayout()
  }
}
