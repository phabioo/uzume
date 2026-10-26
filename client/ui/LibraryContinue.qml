import QtQuick
import QtQuick.Layouts
import Uzume.App

// 3c-0: "Continue" block above the Library table. The last played game (title, meta, primary Continue button that starts
// it like Play) and up to two RECENT entries. Hidden while nothing was played (3c-4 state C). Data: LibraryModel.continueEntries.
Item {
    id: root
    objectName: "libraryContinue"
    required property PlayerController player
    readonly property var entries: root.player.library.continueEntries
    readonly property var last: entries.length > 0 ? entries[0] : null
    readonly property var recent: entries.slice(1, 3)
    // The same path as the Play button of the detail pane: select the game, then play it.
    signal startRequested(string gameId)
    function start(gameId) {
        root.player.selectGame(gameId)
        root.player.playSelected()
        root.startRequested(gameId)
    }

    visible: last !== null
    implicitHeight: visible ? box.implicitHeight : 0
    Layout.preferredHeight: implicitHeight

    Rectangle {
        id: box
        width: parent.width
        implicitHeight: row.implicitHeight + 2 * Theme.space2
        color: Theme.surface
        border.width: 1
        border.color: Theme.rule
        radius: 0

        RowLayout {
            id: row
            anchors.fill: parent
            anchors.margins: Theme.space2
            spacing: Theme.space3

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.space05
                UzSectionLabel { Layout.fillWidth: true; text: qsTr("Continue") }
                UzLabel {
                    objectName: "continueTitle"
                    Layout.fillWidth: true
                    text: root.last ? root.last.title : ""
                    font.pixelSize: Theme.roles.input.size
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
                UzLabel {
                    objectName: "continueMeta"
                    Layout.fillWidth: true
                    text: root.last ? root.last.meta : ""
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta11.size
                    color: Theme.dim
                    elide: Text.ElideRight
                }
            }

            UzButton {
                objectName: "continueButton"
                kind: "primary"
                text: qsTr("Continue")
                onClicked: root.start(root.last.gameId)
            }

            ColumnLayout {
                visible: root.recent.length > 0
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                spacing: Theme.space05
                UzSectionLabel { Layout.fillWidth: true; text: qsTr("Recent") }
                Repeater {
                    model: root.recent
                    delegate: Item {
                        id: entry
                        required property var modelData
                        objectName: "recentEntry"
                        Layout.fillWidth: true
                        implicitHeight: entryCol.implicitHeight
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: entry.modelData.title
                        Accessible.onPressAction: root.start(entry.modelData.gameId)
                        Keys.onPressed: (event) => {
                            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                                root.start(entry.modelData.gameId)
                                event.accepted = true
                            }
                        }
                        Column {
                            id: entryCol
                            width: parent.width
                            UzLabel {
                                width: parent.width
                                text: entry.modelData.title
                                color: hover.hovered ? Theme.accentText : Theme.ink
                                elide: Text.ElideRight
                            }
                            UzLabel {
                                width: parent.width
                                text: entry.modelData.system + " · " + entry.modelData.dateText
                                font.family: Theme.mono
                                font.pixelSize: Theme.roles.meta11.size
                                font.capitalization: Font.AllUppercase
                                color: Theme.dim
                                elide: Text.ElideRight
                            }
                        }
                        UzFocusRing { visible: entry.activeFocus; inset: true }
                        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: root.start(entry.modelData.gameId) }
                    }
                }
            }
        }
    }
}
