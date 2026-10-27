import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8a-5 Set up a server, step 4 "Your admin account": Your name and Password (admins always have one, at least 12 characters).
// Server name and Who can join have no call behind them yet; they are set in the server's settings afterwards.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    property string localError: ""
    screenLabel: qsTr("Step 4 of 4 · Your account")
    openLabel: qsTr("UZUME · SET UP A SERVER")
    initialFocus: nameInput.field
    hints: [{ kbd: "Tab", label: qsTr("Next") }, { kbd: "Enter", label: qsTr("Create server") },
            { pad: "A", label: qsTr("Choose") }, { pad: "Y", label: qsTr("Keyboard") },
            { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: ob.back()

    function submit() {
        if (passwordInput.text.length < 12) {
            root.localError = qsTr("At least 12 characters.")
            return
        }
        root.localError = ""
        root.ob.createAdmin(nameInput.text.trim(), passwordInput.text)
    }

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Set up %1").arg(root.ob.serverName)
        meta: qsTr("Step 4 of 4")
        title: qsTr("Your admin account")
        text: qsTr("The admin manages games, people and settings on the server.")
    }
    StartField {
        id: nameInput
        name: "setupAdminName"
        label: qsTr("Your name")
        error: root.ob.errorCode === "display_name_taken" || root.ob.errorCode === "invalid_input" ? root.ob.errorMessage : ""
        onAccepted: passwordInput.field.forceActiveFocus()
    }
    StartField {
        id: passwordInput
        name: "setupAdminPassword"
        label: qsTr("Password")
        secret: true
        error: root.localError !== "" ? root.localError : (root.ob.errorCode === "bad_request" ? root.ob.errorMessage : "")
        hint: qsTr("Required for admins. At least 12 characters.")
        onAccepted: root.submit()
    }
    StartNotice {
        code: ["display_name_taken", "invalid_input", "bad_request"].indexOf(root.ob.errorCode) >= 0 ? "" : root.ob.errorCode
        message: root.ob.errorMessage
        retryAfter: root.ob.retryAfter
    }
    Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: qsTr("Set the server name and who can join later in the server's settings.")
        textFormat: Text.PlainText
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        color: Theme.muted
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            objectName: "setupCreate"
            kind: "primary"
            busy: root.ob.busy
            enabled: !root.ob.busy
            text: root.ob.busy ? qsTr("Setting up…") : qsTr("Create server")
            onClicked: root.submit()
        }
        UzButton { objectName: "setupAdminBack"; enabled: !root.ob.busy; text: qsTr("Back"); onClicked: root.ob.back() }
        Item { Layout.fillWidth: true }
    }
}
