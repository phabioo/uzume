import QtQuick
import QtQuick.Layouts
import Uzume.App

// Server › Settings › Cheat database (design 11h-1): the block of CheatDbBlock inside the settings pane (GET/PUT /admin/cheat-db).
// objectNames: settingsCheatDb (plus the ones of CheatDbBlock).
ColumnLayout {
    id: root
    objectName: "settingsCheatDb"
    required property var pane
    spacing: 0

    Component.onCompleted: root.pane.helpItems = [
        { title: qsTr("What this is"), text: qsTr("A list of known cheats per game, matched by the game file. Cheats themselves come from the official Cheats plugin, which people add to their games.") },
        { title: qsTr("Offline"), text: qsTr("With the database off, people can still enter and import their own cheats. The last downloaded copy stays in use if an update fails.") }
    ]
    CheatDbBlock {
        id: block
        player: root.pane.player
        pcOnly: root.pane.player.localHub.onThisPcOnly
        onDbChanged: root.pane.cheatDbOn = block.db.enabled === true
    }
}
