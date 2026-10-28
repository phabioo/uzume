import QtQuick
import QtQuick.Layouts
import Uzume.App

// Controller hand-over card (ADR 0033 D8, boards 3g-12 / 3r): on the host's stage, one flat line. A request reads "max wants to play as P2"
// with Allow (key 1) and Deny (key 2) and the seconds left (it is denied after 20 s); while somebody plays it reads "max plays P1" with
// "Take back" (Esc twice does the same for P1). The buttons never take the focus from the game.
Rectangle {
    id: root
    required property ControlController control
    objectName: "controlCard"
    visible: root.control.askOpen || root.control.hasHolder
    implicitHeight: row.implicitHeight + 2 * Theme.space15
    implicitWidth: row.implicitWidth + 2 * Theme.space15
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.Pane
    Accessible.name: root.control.askOpen ? root.control.askText : root.control.holderText

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.space15
        spacing: Theme.space1
        Text {
            objectName: "controlCardText"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            text: root.control.askOpen ? root.control.askText : root.control.holderText
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            font.weight: Font.Medium
            color: Theme.ink
        }
        Text {
            objectName: "controlCardSeconds"
            visible: root.control.askOpen
            text: qsTr("%1 s").arg(root.control.askSeconds)
            font.family: Theme.mono
            font.pixelSize: Theme.roles.meta.size
            color: Theme.muted
        }
        UzButton {
            objectName: "controlAllow"
            visible: root.control.askOpen
            size: "sm"
            kind: "primary"
            focusPolicy: Qt.NoFocus
            text: qsTr("Allow")
            value: "1"
            onClicked: root.control.allow()
        }
        UzButton {
            objectName: "controlDeny"
            visible: root.control.askOpen
            size: "sm"
            focusPolicy: Qt.NoFocus
            text: qsTr("Deny")
            value: "2"
            onClicked: root.control.deny()
        }
        UzButton {
            objectName: "controlTakeBack"
            visible: !root.control.askOpen && root.control.hasHolder
            size: "sm"
            focusPolicy: Qt.NoFocus
            text: qsTr("Take back")
            onClicked: root.control.takeBack()
        }
    }
}
