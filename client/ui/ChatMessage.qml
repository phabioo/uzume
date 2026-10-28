import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// One chat message (board 3g-7): the person colour square, the name and the time on the head line (shown for the first message of a
// run of the same person), the text below it, wrapping. A message that names me has a Yamabuki start rule and the @name in bold; a
// message the Hub has not echoed yet is muted ("Sending…" is not written, the dimming says it), a failed one says "Not sent · Retry".
// `row` is a row of ChatController.rows: {key, clientId, name, color, time, html, plain, mine, mention, pending, failed, head}.
Item {
    id: root
    required property var row
    signal retryRequested(string clientId)
    readonly property real topPad: row.head ? Theme.space15 : 2
    implicitHeight: topPad + col.implicitHeight + 2
    Accessible.role: Accessible.StaticText
    Accessible.name: row.name + ", " + row.time + ": " + row.plain

    Rectangle {
        objectName: "chatMentionRule"
        visible: root.row.mention
        x: Theme.space15 - 2
        y: root.topPad
        width: 2
        height: col.implicitHeight
        color: Theme.accent
    }
    Column {
        id: col
        x: Theme.space3
        y: root.topPad
        width: root.width - 2 * Theme.space3
        spacing: 2
        RowLayout {
            width: parent.width
            visible: root.row.head
            spacing: Theme.space1
            Rectangle {
                objectName: "chatPersonSquare"
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                color: Theme.playerColor(root.row.color, Theme.dark)
            }
            Text {
                objectName: "chatName"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: root.row.name
                textFormat: Text.PlainText
                font.family: Theme.sans
                font.pixelSize: Theme.roles.buttonSm.size
                font.weight: Font.Medium
                color: Theme.ink
            }
            UzMono {
                objectName: "chatTime"
                text: root.row.time
                font.pixelSize: 10
                color: Theme.dim
            }
        }
        Text {
            objectName: "chatText"
            width: parent.width
            text: root.row.html
            textFormat: Text.StyledText
            wrapMode: Text.Wrap
            font.family: Theme.sans
            font.pixelSize: Theme.roles.buttonSm.size
            lineHeight: Theme.roles.small.line + 2
            lineHeightMode: Text.FixedHeight
            color: root.row.pending || root.row.failed ? Theme.muted : Theme.ink
        }
        Text {
            objectName: "chatNotSent"
            visible: root.row.failed
            width: parent.width
            text: qsTr("Not sent · Retry")
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.danger
            TapHandler { onTapped: root.retryRequested(root.row.clientId) }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
