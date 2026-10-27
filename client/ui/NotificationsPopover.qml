import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Notifications popover of the bell (board 6r-2, ADR 0033 D2): the last entries of this server, newest first. Each entry is its text and
// a Mono line "time · ACTION"; one click runs the action and closes. Opening marks everything read.
UzPopover {
    id: root
    required property PlayerController player
    objectName: "notificationsPopover"
    title: qsTr("Notifications")
    popWidth: 340
    property real nowMs: Date.now()
    readonly property var entries: root.player.notifications.entries

    function agoText(ms: real): string {
        const m = Math.floor(Math.max(0, root.nowMs - ms) / 60000)
        if (m < 1) return qsTr("just now")
        if (m < 60) return qsTr("%1 min ago").arg(m)
        const h = Math.floor(m / 60)
        if (h < 24) return qsTr("%1 h ago").arg(h)
        return qsTr("%1 d ago").arg(Math.floor(h / 24))
    }
    onAboutToShow: nowMs = Date.now()
    onOpened: root.player.notifications.markAllRead()

    UzLabel {
        objectName: "notificationsEmpty"
        visible: root.entries.length === 0
        Layout.fillWidth: true
        text: qsTr("Nothing new. Invites, Sessions that start and run news appear here.")
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
    }
    Flickable {
        id: scroller
        visible: root.entries.length > 0
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(column.implicitHeight, 360)
        Layout.leftMargin: -Theme.space2
        Layout.rightMargin: -Theme.space2
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        Column {
            id: column
            width: scroller.width
            Repeater {
                model: root.entries
                delegate: Rectangle {
                    id: entry
                    required property var modelData
                    objectName: "notificationEntry"
                    width: column.width
                    implicitHeight: entryCol.implicitHeight + 2 * Theme.space15
                    height: implicitHeight
                    color: hover.hovered || activeFocus ? Theme.hover : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: entry.modelData.text
                    function run() {
                        root.player.notifications.act(entry.modelData.index)
                        root.close()
                    }
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) { entry.run(); event.accepted = true }
                    }
                    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule2; visible: entry.modelData.index > 0 }
                    Column {
                        id: entryCol
                        x: Theme.space2
                        y: Theme.space15
                        width: parent.width - 2 * Theme.space2
                        spacing: 2
                        Text {
                            objectName: "notificationText"
                            width: parent.width
                            wrapMode: Text.WordWrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: entry.modelData.text
                            font.family: Theme.sans
                            font.pixelSize: Theme.roles.small.size
                            color: Theme.ink
                        }
                        UzMono {
                            objectName: "notificationMeta"
                            width: parent.width
                            elide: Text.ElideRight
                            text: entry.modelData.actionLabel !== "" ? root.agoText(entry.modelData.atMs) + " · " + entry.modelData.actionLabel : root.agoText(entry.modelData.atMs)
                            font.pixelSize: Theme.roles.meta11.size
                            color: Theme.muted
                        }
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: entry.run() }
                    UzFocusRing { visible: entry.activeFocus; inset: true }
                }
            }
        }
    }
}
