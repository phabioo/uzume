import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Content of a plugin tab in the game panel (docs/design/legacy/plugins.md 4e): the plugin's "panel" tree in the active theme under a
// "DRAWN BY <PLUGIN>" line (play-and-plugins.md, boards 5b-1 / 5b-2). A paused
// plugin keeps its last tree, dimmed and without input, under a "Paused" banner with "Restart plugin".
Item {
    id: root
    required property PlayerController player
    required property var tab                      // entry of PluginsController.panelTabs
    readonly property string pluginId: tab.pluginId || ""
    readonly property bool paused: !!tab.paused
    property var tree: ({})

    function refresh() { root.tree = root.player.plugins.slotTree(root.pluginId, "panel") }
    Component.onCompleted: refresh()
    onPluginIdChanged: refresh()
    Connections {
        target: root.player.plugins
        function onSlotChanged(id, slot) { if (id === root.pluginId && slot === "panel") root.refresh() }
        function onPanelTabsChanged() { root.refresh() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "pluginPausedBanner"
            Layout.fillWidth: true
            visible: root.paused
            implicitHeight: bannerRow.implicitHeight + 2 * Theme.space15
            color: Theme.selected
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.accent }
            RowLayout {
                id: bannerRow
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                anchors.rightMargin: Theme.space15
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
                    objectName: "pluginRestartButton"
                    size: "sm"
                    text: qsTr("Restart plugin")
                    onClicked: root.player.plugins.reloadPlugin(root.pluginId)
                }
            }
        }

        Flickable {
            id: flick
            objectName: "pluginPanelFlick"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: content.implicitHeight + 2 * Theme.space3
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: flick.contentHeight > flick.height + 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

            ColumnLayout {
                id: content
                x: Theme.space3
                y: Theme.space3
                width: flick.width - 2 * Theme.space3
                spacing: Theme.space2

                // Plugin content is marked: Mono "DRAWN BY <PLUGIN>" over a rule.
                RowLayout {
                    objectName: "pluginDrawnBy"
                    Layout.fillWidth: true
                    spacing: Theme.space15
                    UzSectionLabel {
                        Layout.maximumWidth: content.width * 0.7
                        text: qsTr("Drawn by %1").arg(root.tab.name || root.pluginId)
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.rule2 }
                }
                PluginBlock {
                    id: block
                    objectName: "pluginPanelBlock"
                    Layout.fillWidth: true
                    Layout.preferredHeight: implicitHeight
                    node: root.tree
                    dark: Theme.dark
                    pluginId: root.pluginId
                    opacity: root.paused ? 0.45 : 1
                    enabled: !root.paused
                    onAction: (a, v) => root.player.plugins.sendAction(root.pluginId, "panel", a, v)
                }
                Text {
                    textFormat: Text.PlainText  // plugin- and peer-supplied strings are never HTML
                    Layout.fillWidth: true
                    visible: !root.paused && root.tree.type === undefined
                    text: qsTr("Nothing to show yet.")
                    wrapMode: Text.WordWrap
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.input.size
                    color: Theme.muted
                }
            }
        }
    }
}
