import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// In-game plugin screen (docs/design/legacy/plugins.md, "In-game screen sheet"): opened by fb.ui.open("screen"). Covers the body
// below the GameHeader (play area and side panel; in fullscreen the play area). In the active theme (the game view is dark). The game keeps running
// underneath. Top bar: "← Back to game", plugin icon, screen title and plugin name, key hint Esc; below it the plugin's
// "screen" tree (max 1100 wide, or 560 with `width = "dialog"` on its root column; centered, scrolling vertically). Esc and Back emit closeRequested().
// The sheet is a focus scope: opening it moves the focus to "Back to game" and Tab / Shift+Tab walk through its own controls (Back,
// the tabs and buttons of the screen); the host keeps what the sheet covers out of the tab chain while it is open.
FocusScope {
    id: root
    objectName: "pluginScreenSheet"
    required property PlayerController player
    property string pluginId: ""
    property string backText: qsTr("← Back to game")
    readonly property bool open: pluginId !== ""
    readonly property var entry: {
        var e = root.player.plugins.sidebarEntries
        for (var i = 0; i < e.length; ++i) if (e[i].pluginId === root.pluginId) return e[i]
        return ({})
    }
    readonly property bool paused: {
        var t = root.player.plugins.panelTabs
        for (var i = 0; i < t.length; ++i) if (t[i].pluginId === root.pluginId) return !!t[i].paused
        return false
    }
    property var tree: ({})
    signal closeRequested()

    visible: open
    clip: true

    function refresh() { root.tree = root.pluginId !== "" ? root.player.plugins.slotTree(root.pluginId, "screen") : ({}) }
    // The focus goes to "Back to game" (first stop of the sheet); Esc still reaches the sheet's key handler from there.
    function takeFocus() { backButton.forceActiveFocus() }
    onPluginIdChanged: { refresh(); if (pluginId !== "") Qt.callLater(root.takeFocus) }
    Component.onCompleted: refresh()
    Connections {
        target: root.player.plugins
        function onSlotChanged(id, slot) { if (id === root.pluginId && slot === "screen") root.refresh() }
        function onSidebarEntriesChanged() {
            root.refresh()
            // the plugin stopped or was removed: nothing left to show
            if (root.open && root.entry.pluginId === undefined) root.closeRequested()
        }
    }

    Keys.onEscapePressed: (e) => { e.accepted = true; if (!e.isAutoRepeat) root.closeRequested() }

    Rectangle { anchors.fill: parent; color: Theme.ground }

    // The sheet is opaque to the play area below
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (w) => w.accepted = true }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.header56
            color: Theme.ground
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space15
                anchors.rightMargin: Theme.space2
                spacing: Theme.space15
                UzButton {
                    id: backButton
                    objectName: "pluginScreenBack"
                    kind: "ghost"
                    size: "sm"
                    text: root.backText
                    onClicked: root.closeRequested()
                }
                PluginTabIcon {
                    size: 24
                    name: root.entry.name || ""
                    icon: root.entry.icon || ""
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    objectName: "pluginScreenSheetTitle"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    text: (root.entry.title || root.entry.name || "") + (root.entry.name && root.entry.title && root.entry.title !== root.entry.name ? "  ·  " + root.entry.name : "")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.headerTitle.size
                    font.weight: Theme.roles.headerTitle.weight
                    color: Theme.ink
                }
                UzKbd { keys: "Esc" }
            }
        }

        Rectangle {
            objectName: "pluginScreenSheetPaused"
            Layout.fillWidth: true
            visible: root.paused
            implicitHeight: bannerRow.implicitHeight + 2 * Theme.space15
            color: Theme.selected
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.accent }
            RowLayout {
                id: bannerRow
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space2
                spacing: Theme.space15
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    Layout.fillWidth: true
                    text: qsTr("Paused · showing its last state")
                    wrapMode: Text.WordWrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.small.size
                    font.weight: Font.Medium
                    color: Theme.accentText
                }
                UzButton {
                    size: "sm"
                    text: qsTr("Restart plugin")
                    onClicked: root.player.plugins.reloadPlugin(root.pluginId)
                }
            }
        }

        Flickable {
            id: flick
            objectName: "pluginScreenSheetFlick"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: content.implicitHeight + 2 * Theme.space3
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: content
                readonly property real avail: flick.width - 2 * Theme.space3
                // 1100 px, or dialog width (560) when the root column of the screen says `width = "dialog"` (forms such as "Add cheats").
                width: Math.min(root.tree.width === "dialog" ? Theme.dialogLg : 1100, Math.max(0, avail))
                x: Math.max(Theme.space3, (flick.width - width) / 2)
                y: Theme.space3
                spacing: Theme.space2

                // No "Drawn by" line here: the header above already names the plugin (and its origin).
                PluginBlock {
                    id: block
                    objectName: "pluginScreenSheetBlock"
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    node: root.tree
                    dark: Theme.dark
                    pluginId: root.pluginId
                    opacity: root.paused ? 0.45 : 1
                    enabled: !root.paused
                    onAction: (a, v) => root.player.plugins.sendAction(root.pluginId, "screen", a, v)
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    Layout.fillWidth: true
                    visible: !root.paused && root.tree.type === undefined
                    text: qsTr("Nothing to show yet.")
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.muted
                }
            }
        }
    }
}
