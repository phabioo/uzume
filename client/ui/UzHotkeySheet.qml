import QtQuick
import QtQuick.Layouts

// Hotkey sheet (design-system.md "HotkeySheet"): the "Hold Select +" grid over the stage: 400 px on `surface` in a `rule` frame;
// head with the pressed Select key and a Mono label, two columns of GamepadHint + action, a foot "Release Select to close".
// keys: [[key, label], [key, label, whyDisabled]]; a third entry greys the row and shows the reason at the right.
// modifier: the hotkey button that is held ("Select" unless the player chose another one).
Rectangle {
    id: root
    property string modifier: "Select"
    property var keys: [["A", qsTr("Save state")], ["B", qsTr("Load state")], ["L", qsTr("Previous slot")], ["R", qsTr("Next slot")],
                        ["Y", qsTr("Screenshot")], ["X", qsTr("Mute")], ["UpDown", qsTr("Volume")], ["Start", qsTr("Pause menu")]]
    implicitWidth: 400
    implicitHeight: headBox.height + gridBox.implicitHeight + footBox.height + 2
    radius: 0
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("%1 shortcuts").arg(root.modifier)

    Item {
        id: headBox
        x: 1; y: 1
        width: parent.width - 2
        height: 48
        Row {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space1
            UzGamepadHint { k: root.modifier; pressed: true; anchors.verticalCenter: parent.verticalCenter }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Hold %1 +").arg(root.modifier)
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                font.weight: Font.Medium
                font.letterSpacing: 1.4
                font.capitalization: Font.AllUppercase
                color: Theme.muted
            }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
    }
    GridLayout {
        id: gridBox
        objectName: "hotkeyGrid"
        x: 1 + Theme.space2
        y: 1 + headBox.height + Theme.space15
        width: parent.width - 2 - 2 * Theme.space2
        columns: 2
        columnSpacing: Theme.space3
        rowSpacing: Theme.space1
        Repeater {
            model: root.keys
            delegate: RowLayout {
                id: hk
                required property var modelData
                readonly property bool off: modelData.length > 2
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 0
                spacing: Theme.space1
                UzGamepadHint { k: hk.modelData[0]; disabled: hk.off }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    text: hk.modelData[1]
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: hk.off ? Theme.disabledInk : Theme.ink
                }
                Text {
                    visible: hk.off
                    text: hk.off ? hk.modelData[2] : ""
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    font.letterSpacing: 0.6
                    font.capitalization: Font.AllUppercase
                    color: Theme.muted
                }
            }
        }
    }
    Item {
        id: footBox
        x: 1
        y: parent.height - height - 1
        width: parent.width - 2
        height: 36
        Rectangle { width: parent.width; height: 1; color: Theme.rule2 }
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space2
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Release %1 to close").arg(root.modifier)
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            font.letterSpacing: 0.6
            font.capitalization: Font.AllUppercase
            color: Theme.muted
        }
    }
}
