import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The games a participant may bring into a run (RunsController.games): hairline radio rows under a `rule` line, one selected.
// The list scrolls inside a fixed height (6 rows) when there are many games; with more than 6 games a search field filters it.
Item {
    id: root
    required property var rows           // RunsController.games
    property string selectedId: ""
    readonly property bool searchable: root.rows.count > 6
    property string query: ""
    readonly property int searchH: root.searchable ? Theme.row36 + Theme.space1 : 0
    implicitHeight: root.rows.count === 0 ? Theme.row44 * 2 : root.searchH + Math.min(Theme.row44 * 6 + 8, listCol.implicitHeight + 1)
    clip: true

    UzField {
        id: searchField
        objectName: root.objectName + "Search"
        visible: root.searchable
        anchors.top: parent.top
        width: parent.width
        height: Theme.row36
        placeholderText: qsTr("Search games")
        onTextChanged: root.query = text.trim().toLowerCase()
    }
    Rectangle { anchors.top: parent.top; anchors.topMargin: root.searchH; width: parent.width; height: 1; color: Theme.rule }
    UzLabel {
        anchors.centerIn: parent
        visible: root.rows.count === 0
        width: parent.width - 2 * Theme.space15
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("No game in your library fits this run.")
        color: Theme.muted
        font.pixelSize: Theme.roles.input.size
    }
    Flickable {
        anchors.fill: parent
        anchors.topMargin: root.searchH + 1
        clip: true
        contentWidth: width
        contentHeight: listCol.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: listCol
            width: parent.width
            spacing: 0
            Repeater {
                model: root.rows
                delegate: RunCheckRow {
                    id: gameRow
                    required property var model
                    objectName: "runGame_" + gameRow.model.id
                    visible: root.query === "" || String(gameRow.model.title).toLowerCase().indexOf(root.query) >= 0
                    radio: true
                    title: gameRow.model.title
                    sub: gameRow.model.hint !== "" ? gameRow.model.system.toUpperCase() + " · " + gameRow.model.hint : gameRow.model.system.toUpperCase()
                    checked: root.selectedId === gameRow.model.id
                    onToggled: root.selectedId = gameRow.model.id
                }
            }
        }
    }
}
