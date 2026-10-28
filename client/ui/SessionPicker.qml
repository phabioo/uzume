import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Running sessions (board 3r-5, "Add tile" popover): "Running sessions" with "N of 4 tiles", a hairline row (44 px) per
// watchable session with "Add" or the "Added" tag, the empty state (StateBlock), the full note and the footer note. It never
// opens on its own; the owner shows it in an UzPopover.
ColumnLayout {
    id: root
    required property PlayerController player
    readonly property SessionController ctl: player.sessions
    property real maxListHeight: 260
    readonly property var list: ctl.sessions
    readonly property var shown: ctl.shownSessionIds
    readonly property int count: ctl.surfaceCount
    readonly property bool full: count >= ctl.maxSurfaces
    objectName: "multiviewSessionList"
    spacing: Theme.space15

    function firstFree() {
        for (var i = 0; i < list.length; ++i) {
            if (shown.indexOf(list[i].sessionId) < 0) return list[i].title
        }
        return ""
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        Text {
            objectName: "multiviewListTitle"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            elide: Text.ElideRight
            textFormat: Text.PlainText
            text: qsTr("Running sessions")
            font.family: Theme.sans
            font.pixelSize: Theme.roles.itemTitle.size
            font.weight: Theme.roles.itemTitle.weight
            color: Theme.ink
        }
        UzMono {
            objectName: "multiviewListCount"
            // The counter keeps its full width; the "Running sessions" title (fillWidth, elides) gives way first
            Layout.minimumWidth: implicitWidth
            text: qsTr("%1 of %2 tiles").arg(root.count).arg(root.ctl.maxSurfaces)
            font.pixelSize: 10
            font.letterSpacing: 0.8
            font.capitalization: Font.AllUppercase
            color: Theme.dim
        }
    }

    UzStateBlock {
        objectName: "multiviewNoSessions"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.list.length === 0
        kind: "empty"
        title: qsTr("No other sessions right now")
        text: qsTr("Sessions appear here when someone on %1 shares a game with everyone here or invites you.").arg(root.player.hubAddress || qsTr("your server"))
    }

    Item {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.list.length > 0
        implicitHeight: pickView.height + 1
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.rule }
        ListView {
            id: pickView
            objectName: "multiviewSessionView"
            y: 1
            width: parent.width
            height: Math.min(contentHeight, root.maxListHeight)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.list
            ScrollBar.vertical: ScrollBar { policy: pickView.contentHeight > pickView.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }
            delegate: Item {
                id: prow
                required property var modelData
                required property int index
                readonly property bool isShown: root.shown.indexOf(modelData.sessionId) >= 0
                readonly property int tileNo: root.ctl.surfaceOrder.indexOf(modelData.sessionId) + 1
                objectName: "multiviewSession_" + modelData.sessionId
                width: ListView.view.width - (pickView.contentHeight > pickView.height ? 10 : 0)
                implicitHeight: Theme.row44
                height: Theme.row44
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    spacing: Theme.space15
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 0
                        spacing: 0
                        UzLabel { Layout.fillWidth: true; elide: Text.ElideRight; text: prow.modelData.title; font.pixelSize: Theme.roles.itemTitleSm.size; color: Theme.ink }
                        UzLabel {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: prow.isShown ? prow.modelData.meta + qsTr(" · in tile %1").arg(prow.tileNo) : prow.modelData.meta
                            font.pixelSize: Theme.roles.small.size
                            color: prow.modelData.invited && !prow.isShown ? Theme.accentText : Theme.muted
                        }
                    }
                    UzTag {
                        objectName: "multiviewAdded_" + prow.modelData.sessionId
                        visible: prow.isShown
                        tone: "ok"
                        text: qsTr("Added")
                    }
                    UzButton {
                        objectName: "multiviewAddButton_" + prow.modelData.sessionId
                        visible: !prow.isShown
                        size: "sm"
                        focusPolicy: Qt.NoFocus
                        text: qsTr("Add")
                        enabled: root.ctl.hubLink === "online" && root.ctl.canAddSurface && !prow.isShown
                        onClicked: root.ctl.watch(prow.modelData.sessionId)
                    }
                }
            }
        }
    }

    ColumnLayout {
        objectName: "multiviewFullNote"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.full
        spacing: Theme.space05
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: qsTr("Multiview is full")
            font.family: Theme.sans
            font.pixelSize: Theme.roles.itemTitle.size
            font.weight: Theme.roles.itemTitle.weight
            color: Theme.ink
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.ink2
            text: root.firstFree() !== "" ? qsTr("Remove a tile to add %1.").arg(root.firstFree())
                                          : qsTr("Remove a tile to add another session.")
        }
    }
    Text {
        objectName: "multiviewListNote"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
        text: qsTr("Private sessions are not listed. Sound plays from one tile unless Mix all tiles is on (Mixer).")
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
}
