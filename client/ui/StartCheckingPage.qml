import QtQuick
import QtQuick.Layouts
import Uzume.App

// Contacting the server (identify, certificate): a short waiting state with Cancel.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    screenLabel: ob.flow === "setup" ? qsTr("Step 2 of 4 · Connect to the device") : qsTr("Step 1 of 3 · Server")
    openLabel: ob.flow === "setup" ? qsTr("UZUME · SET UP A SERVER") : qsTr("UZUME · JOIN A SERVER")
    initialFocus: cancel
    hints: [{ kbd: "Esc", pad: "B", label: qsTr("Cancel") }]
    onBackRequested: ob.cancel()

    StartHead {
        step: root.screenLabel
        number: 1
        label: root.ob.flow === "setup" ? qsTr("Set up a server") : qsTr("Join a server")
        title: qsTr("Contacting the server")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space15
        UzSpinner { objectName: "startChecking"; size: 16; Layout.alignment: Qt.AlignVCenter }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.ob.serverAddress !== "" ? qsTr("Waiting for %1…").arg(root.ob.serverAddress) : qsTr("Waiting for the server…")
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.address.size
            color: Theme.ink2
        }
    }
    RowLayout {
        Layout.fillWidth: true
        UzButton { id: cancel; objectName: "checkingCancel"; text: qsTr("Cancel"); onClicked: root.ob.cancel() }
        Item { Layout.fillWidth: true }
    }
}
