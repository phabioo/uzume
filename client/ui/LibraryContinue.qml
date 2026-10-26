import QtQuick
import QtQuick.Layouts
import Uzume.App

// 3c-0: "Continue" section above the Library table: up to three compact cards (64 px: system tile, title, "SYSTEM · when" meta,
// "Play" link) in one row, the last played game first. Hidden while nothing was played (3c-4 state C). Data:
// LibraryModel.continueEntries. A card starts its game like the Play button of the detail pane.
ColumnLayout {
    id: root
    objectName: "libraryContinue"
    required property PlayerController player
    readonly property var entries: root.player.library.continueEntries
    readonly property var shown: entries.slice(0, 3)
    // The same path as the Play button of the detail pane: select the game, then play it.
    signal startRequested(string gameId)
    function start(gameId) {
        root.player.selectGame(gameId)
        root.player.playSelected()
        root.startRequested(gameId)
    }

    visible: shown.length > 0
    spacing: Theme.space1

    UzSectionLabel { objectName: "continueLabel"; Layout.fillWidth: true; number: 1; text: qsTr("Continue") }

    RowLayout {
        id: cards
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space2
        Repeater {
            model: root.shown
            delegate: Rectangle {
                id: card
                required property var modelData
                required property int index
                objectName: index === 0 ? "continueCard" : "recentEntry"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.preferredWidth: 1
                Layout.maximumWidth: 360   // fewer than three cards stay card-sized
                Layout.preferredHeight: 64
                color: hover.hovered ? Theme.hover : Theme.surface
                border.width: 1
                border.color: card.activeFocus ? Theme.ink : Theme.rule
                radius: 0
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: card.modelData.title
                Accessible.onPressAction: root.start(card.modelData.gameId)
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                        root.start(card.modelData.gameId)
                        event.accepted = true
                    }
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space1
                    spacing: Theme.space15
                    Rectangle {   // system tile
                        Layout.preferredWidth: 40
                        Layout.preferredHeight: 40
                        color: Theme.ground
                        border.width: 1
                        border.color: Theme.rule
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 4
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: card.modelData.system
                            textFormat: Text.PlainText
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.systemCode.size
                            font.weight: Font.Medium
                            color: Theme.ink2
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 0
                        UzLabel {
                            objectName: index === 0 ? "continueTitle" : ""
                            Layout.fillWidth: true
                            text: card.modelData.title
                            font.pixelSize: Theme.roles.input.size
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        UzLabel {
                            objectName: index === 0 ? "continueMeta" : ""
                            Layout.fillWidth: true
                            text: card.modelData.system + " · " + card.modelData.dateText
                            font.family: Theme.mono
                            font.pixelSize: Theme.roles.meta11.size
                            font.capitalization: Font.AllUppercase
                            color: Theme.dim
                            elide: Text.ElideRight
                        }
                    }
                    UzButton {
                        objectName: index === 0 ? "continueButton" : ""
                        kind: "link"
                        text: qsTr("Play")
                        onClicked: root.start(card.modelData.gameId)
                    }
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.start(card.modelData.gameId) }
            }
        }
    }
}
