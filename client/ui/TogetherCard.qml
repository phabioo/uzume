import QtQuick
import QtQuick.Layouts
import Uzume.App

// Ready check card (ADR 0033 D3, board 3g / 10f-5e): who started it, one row per member with the state as a word and a mark (never
// colour alone), the seconds left and the two answers Ready (1) / Not yet (2); the starter also sees Cancel. Flat, one frame, no
// nesting. On the stage it floats top centre; `inline` puts it into the run board. Keys 1 / 2 and the pad (A / B) answer in the
// game (GameScreen, PlayerController); the buttons never take the focus from the game.
Rectangle {
    id: root
    required property TogetherController together
    property bool inline: false
    objectName: "readyCheckCard"
    visible: root.together.checkOpen
    implicitHeight: content.implicitHeight + 2 * Theme.space15
    implicitWidth: inline ? 360 : 340
    color: Theme.surface
    border.width: 1
    border.color: Theme.rule
    Accessible.role: Accessible.Pane
    Accessible.name: root.together.checkTitle

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: Theme.space15
        spacing: Theme.space1

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space1
            Text {
                objectName: "readyCheckTitle"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: root.together.checkTitle
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.small.size
                font.weight: Font.Medium
                color: Theme.ink
            }
            Text {
                objectName: "readyCheckSeconds"
                text: qsTr("%1 s").arg(root.together.secondsLeft)
                font.family: Theme.mono
                font.pixelSize: Theme.roles.meta.size
                color: Theme.muted
            }
        }

        Repeater {
            model: root.together.checkMembers
            delegate: RowLayout {
                id: member
                required property var modelData
                objectName: "readyCheckMember"
                Layout.fillWidth: true
                spacing: Theme.space1
                UzAvatar { name: member.modelData.name; person: member.modelData.color; size: 20 }
                Text {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    text: member.modelData.me ? qsTr("%1 (you)").arg(member.modelData.name) : member.modelData.name
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    color: Theme.ink
                }
                Text {
                    objectName: "readyCheckState"
                    text: member.modelData.mark + " " + member.modelData.stateText
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: member.modelData.state === "pending" ? Font.Normal : Font.Medium
                    color: member.modelData.state === "pending" ? Theme.muted : Theme.ink
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space05
            spacing: Theme.space1
            UzButton {
                objectName: "readyCheckReady"
                size: "sm"
                kind: "primary"
                focusPolicy: Qt.NoFocus
                text: qsTr("Ready")
                value: root.inline ? "" : "1"
                onClicked: root.together.answer(true)
            }
            UzButton {
                objectName: "readyCheckNotYet"
                size: "sm"
                focusPolicy: Qt.NoFocus
                text: qsTr("Not yet")
                value: root.inline ? "" : "2"
                on: root.together.myState === "not_ready"
                onClicked: root.together.answer(false)
            }
            Item { Layout.fillWidth: true }
            UzButton {
                objectName: "readyCheckCancel"
                visible: root.together.iStarted
                size: "sm"
                kind: "link"
                focusPolicy: Qt.NoFocus
                text: qsTr("Cancel")
                onClicked: root.together.cancelCheck()
            }
        }
    }
}
