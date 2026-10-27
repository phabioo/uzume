import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 8a-4 "On this PC": the existing path of LocalHubController (install the server when none is registered, connect over
// loopback, create the admin or sign in as one). Skips steps 2 and 3. Cancel / Esc stops it.
StartPage {
    id: root
    required property PlayerController player
    readonly property var local: player.localHub
    readonly property bool form: local.phase === "needsSetup" || local.phase === "needsSignIn" || local.phase === "working"
    signal leave()
    screenLabel: qsTr("Set up a server on this PC")
    openLabel: qsTr("UZUME · SET UP A SERVER")
    initialFocus: form ? null : cancelButton
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Choose") }, { kbd: "Esc", pad: "B", label: qsTr("Cancel") }]
    onBackRequested: root.cancelSetup()

    function cancelSetup() {
        root.local.cancel()
        root.leave()
    }

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Set up a server")
        meta: qsTr("On this PC")
        title: root.local.phase === "installing" ? qsTr("Installing the server")
               : root.local.phase === "needsSetup" ? qsTr("Create the server admin")
               : root.local.phase === "needsSignIn" ? qsTr("Sign in as a server admin")
               : root.local.phase === "error" ? qsTr("The server could not be set up")
               : qsTr("Connecting to this PC")
    }
    RowLayout {
        objectName: "setupLocalProgress"
        visible: root.local.phase === "installing" || root.local.phase === "connecting" || root.local.phase === "idle"
        Layout.fillWidth: true
        spacing: Theme.space15
        UzSpinner { size: 16; Layout.alignment: Qt.AlignVCenter }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: root.local.phase === "installing" && root.local.installText !== "" ? root.local.installText
                  : root.local.phase === "installing" ? qsTr("Installing the Uzume server on this PC…")
                  : qsTr("Connecting to the server on this PC…")
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.body.size
            color: Theme.ink2
        }
    }
    UzProgress {
        objectName: "setupLocalInstallProgress"
        visible: root.local.phase === "installing" && root.local.installProgress > 0 && root.local.installProgress < 1
        Layout.fillWidth: true
        thin: true
        value: root.local.installProgress
    }
    LocalHubForm {
        objectName: "localHubForm"
        visible: root.form
        Layout.fillWidth: true
        player: root.player
    }
    UzStatusIcon {
        objectName: "setupLocalError"
        visible: root.local.phase === "error" && root.local.error !== ""
        kind: "error"
        text: root.local.error
        maxWidth: 480
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            objectName: "setupLocalRetry"
            visible: root.local.phase === "error"
            kind: "primary"
            text: qsTr("Try again")
            onClicked: root.local.start()
        }
        UzButton {
            id: cancelButton
            objectName: "setupLocalCancel"
            text: root.local.phase === "error" ? qsTr("Back") : qsTr("Cancel")
            onClicked: root.cancelSetup()
        }
        Item { Layout.fillWidth: true }
    }
}
