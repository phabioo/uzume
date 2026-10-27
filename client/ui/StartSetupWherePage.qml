import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8a-4 step 1 "Where should it run?": On this PC (the Hub on this PC, LocalHubController) or On another device (address and
// setup code next). `chose("pc" | "device")`; Back / Esc returns to the first screen.
StartPage {
    id: root
    required property PlayerController player
    signal chose(string where)
    signal leave()
    readonly property bool localAvailable: player.localHub.available
    screenLabel: qsTr("Step 1 of 4 · Where it runs")
    openLabel: qsTr("UZUME · SET UP A SERVER")
    maxWidth: 880
    spacing: 32
    initialFocus: pcRow.enabled ? pcRow : deviceRow
    hints: [{ kbd: "↑ ↓", pad: "UpDown", label: qsTr("Move") },
            { kbd: "Enter", pad: "A", label: qsTr("Choose") },
            { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: root.leave()

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Set up a server")
        meta: qsTr("Step 1 of 4")
        title: qsTr("Where should it run?")
        text: qsTr("On this PC skips steps 2 and 3.")
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
        StartChoiceRow {
            id: pcRow
            objectName: "setupOnThisPc"
            number: "01"
            primaryChoice: true
            enabled: root.localAvailable
            title: qsTr("On this PC")
            text: qsTr("Runs as a background service, also when Uzume is closed. Needs administrator rights once.")
            meta: root.localAvailable ? qsTr("Windows asks once") : qsTr("Not available on this PC")
            KeyNavigation.down: deviceRow
            onActivated: root.chose("pc")
        }
        StartChoiceRow {
            id: deviceRow
            objectName: "setupOnDevice"
            number: "02"
            title: qsTr("On another device")
            text: qsTr("A Raspberry Pi, a NAS with Docker or a Windows server. No screen needed.")
            meta: qsTr("Needs its address and setup code")
            KeyNavigation.up: pcRow
            onActivated: root.chose("device")
        }
    }
    RowLayout {
        Layout.fillWidth: true
        UzButton { objectName: "setupWhereBack"; text: qsTr("Back"); onClicked: root.leave() }
        Item { Layout.fillWidth: true }
    }
}
