import QtQuick
import QtQuick.Layouts
import Uzume.App

// Sessions of other people on the server (Library, hidden when there are none): a section label and a hairline row (56 px) per
// Session with the person's initial, the title, the meta line and Watch Session, or Join and Decline for an invite. Live: the list
// follows the server link, rows appear and disappear while the Library is open.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property var list: player.sessions.sessions
    objectName: "sessionsSection"
    property bool suppressed: false   // the game page is open
    visible: list.length > 0 && !suppressed
    spacing: Theme.space1

    UzSectionLabel {
        Layout.fillWidth: true
        text: qsTr("Sessions")
        meta: qsTr("%1 running").arg(root.list.length)
    }

    Column {
        Layout.fillWidth: true
        Layout.topMargin: Theme.space05
        Rectangle { width: parent.width; height: 1; color: Theme.rule }
        Repeater {
            model: root.list
            delegate: Item {
                id: card
                required property var modelData
                required property int index
                objectName: "sessionCard_" + modelData.sessionId
                width: parent.width
                implicitHeight: Math.max(Theme.row44 + Theme.space1, cardRow.implicitHeight + Theme.space15)
                height: implicitHeight
                opacity: 0
                Component.onCompleted: opacity = 1
                Behavior on opacity { NumberAnimation { duration: Theme.durList } }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                // The start rule marks an invite
                Rectangle { visible: card.modelData.invited; width: 2; height: parent.height - 1; color: Theme.accent }

                RowLayout {
                    id: cardRow
                    anchors.fill: parent
                    anchors.leftMargin: card.modelData.invited ? Theme.space15 : Theme.space05
                    anchors.rightMargin: Theme.space05
                    spacing: Theme.space15
                    UzAvatar {
                        Layout.alignment: Qt.AlignVCenter
                        name: card.modelData.title || ""
                        person: card.index + 2
                        size: 28
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 1
                        spacing: 0
                        UzLabel {
                            objectName: "sessionTitle"
                            Layout.fillWidth: true
                            text: card.modelData.title
                            font.pixelSize: Theme.roles.itemTitle.size
                            font.weight: Theme.roles.itemTitle.weight
                            elide: Text.ElideRight
                        }
                        UzLabel {
                            objectName: "sessionMeta"
                            Layout.fillWidth: true
                            text: card.modelData.meta
                            color: card.modelData.invited ? Theme.accentText : Theme.muted
                            font.pixelSize: Theme.roles.small.size
                            elide: Text.ElideRight
                        }
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Theme.space1
                        UzButton {
                            objectName: (card.modelData.invited ? "join_" : "watch_") + card.modelData.sessionId
                            size: "sm"
                            kind: card.modelData.invited ? "primary" : "outline"
                            busy: root.player.sessions.joining
                            text: card.modelData.invited ? qsTr("Join") : qsTr("Watch Session")
                            enabled: root.player.sessions.hubLink === "online" && !root.player.sessions.joining
                            onClicked: root.player.sessions.watch(card.modelData.sessionId)
                        }
                        UzButton {
                            visible: card.modelData.invited
                            objectName: "decline_" + card.modelData.sessionId
                            size: "sm"
                            kind: "ghost"
                            text: qsTr("Decline")
                            onClicked: root.player.sessions.decline(card.modelData.sessionId)
                        }
                    }
                }
            }
        }
    }
}
