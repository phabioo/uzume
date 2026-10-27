import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8a-1 / 8a-2 First start: three ways to start. Play on this PC, Join a server, Set up a server. `chose("play" | "join" |
// "setup")` goes to the screen; the invite link line at the foot belongs to the window-wide Ctrl+V.
StartPage {
    id: root
    required property PlayerController player
    signal chose(string what)
    screenLabel: qsTr("First start")
    openLabel: qsTr("UZUME · FIRST START")
    maxWidth: 880
    spacing: 32
    initialFocus: playRow
    hints: [{ kbd: "↑ ↓", pad: "UpDown", label: qsTr("Move") },
            { kbd: "Enter", pad: "A", label: qsTr("Choose") },
            { pad: "B", label: qsTr("Back") }]

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Welcome")
        meta: qsTr("3 ways to start")
        title: qsTr("Welcome to Uzume")
        text: qsTr("How do you want to play? You can change this later in Settings › This PC › Servers.")
    }
    ColumnLayout {
        objectName: "startChoices"
        Layout.fillWidth: true
        spacing: 0
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
        StartChoiceRow {
            id: playRow
            objectName: "startPlay"
            number: "01"
            primaryChoice: true
            title: qsTr("Play on this PC")
            text: qsTr("Your games and saves stay on this computer. Nothing to set up, no account. You can share with friends later.")
            meta: qsTr("No admin rights needed")
            KeyNavigation.down: joinRow
            onActivated: root.chose("play")
        }
        StartChoiceRow {
            id: joinRow
            objectName: "startJoin"
            number: "02"
            title: qsTr("Join a server")
            text: qsTr("A friend runs Uzume for a group. You need their address or an invite link.")
            meta: qsTr("Most people in a SoulLink run start here")
            KeyNavigation.up: playRow
            KeyNavigation.down: setupRow
            onActivated: root.chose("join")
        }
        StartChoiceRow {
            id: setupRow
            objectName: "startSetup"
            number: "03"
            title: qsTr("Set up a server")
            text: qsTr("Keep games and saves for friends at home or over the internet. Runs on this PC or on a Raspberry Pi, NAS or another PC.")
            meta: qsTr("You become the admin")
            KeyNavigation.up: joinRow
            onActivated: root.chose("setup")
        }
    }
    RowLayout {
        objectName: "startPasteHint"
        Layout.fillWidth: true
        spacing: Theme.space1
        Text {
            text: qsTr("Have an invite link? Paste it here with")
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
        }
        UzKbd { keys: "Ctrl+V" }
        Item { Layout.fillWidth: true }
    }
}
