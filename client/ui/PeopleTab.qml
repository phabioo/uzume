import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// People tab of the game panel (board 3g-12): who is in this Session with their role, and the two actions "Ready check…" (the ready check
// of the scope, also in the ⋯ menu) and "Invite someone…" (the Sharing section of the Session tab). Only the frame of the board: the voice
// marks come with the voice package. Connection, latency and quality received are on each row (PeopleList); the host sees the line
// "Sending N streams · X Mbit/s" under the list.
Flickable {
    id: root
    required property PlayerController player
    readonly property ChatController chat: player.chat
    property bool canInvite: true
    signal inviteRequested()
    contentWidth: width
    contentHeight: col.implicitHeight + 2 * Theme.space3
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: col
        x: Theme.space3
        y: 20
        width: root.width - 2 * Theme.space3
        spacing: Theme.space2
        PeopleList {
            objectName: "peopleList"
            width: parent.width
            people: root.chat.people
            scopeKind: root.chat.scopeKind
            onQualityPicked: (sessionId, value) => root.player.sessions.setQuality(sessionId, value)
        }
        UzLabel {
            objectName: "peopleSending"
            visible: text !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: root.player.sessions.sendingLine
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        Flow {
            width: parent.width
            spacing: Theme.space1
            UzButton {
                objectName: "peopleReadyCheck"
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Ready check…")
                enabled: root.player.together.available
                onClicked: root.player.together.startCheck()
            }
            UzButton {
                objectName: "peopleInvite"
                visible: root.canInvite
                size: "sm"
                focusPolicy: Qt.TabFocus
                text: qsTr("Invite someone…")
                onClicked: root.inviteRequested()
            }
        }
    }
}
