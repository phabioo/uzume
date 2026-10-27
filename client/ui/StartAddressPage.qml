import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// 8b-1 Join: "Where do you want to go?" One field for an address, an invite code or an `uzume://` link, with the recent
// servers above it as 44 px rows. 8a-4 step 2 (flow "setup"): address and setup code of the device. Errors of the address step
// (broken link, invalid input) sit under the field; the controller's copy is shown as it is.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    readonly property bool setup: ob.flow === "setup"
    readonly property bool setupCodeError: ob.errorCode === "invalid_setup_code" || ob.errorCode === "setup_code_expired"
    screenLabel: setup ? qsTr("Step 2 of 4 · Connect to the device") : qsTr("Step 1 of 3 · Server")
    openLabel: setup ? qsTr("UZUME · SET UP A SERVER") : qsTr("UZUME · JOIN A SERVER")
    initialFocus: setup ? setupAddress.field : joinInput.field
    hints: [{ kbd: "Enter", label: qsTr("Continue") }, { kbd: "Ctrl+V", label: qsTr("Paste a link") },
            { pad: "A", label: qsTr("Choose") }, { pad: "Y", label: qsTr("Keyboard") },
            { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    helpItems: setup ? [] : [
        { title: qsTr("An invite link works here"), text: qsTr("Paste it into the field. The link names the server and carries the invite.") },
        { title: qsTr("Where is the address?"), text: qsTr("The person who runs the server knows it. Admins find it in Server › Settings › Network & sharing.") }
    ]
    onBackRequested: ob.back()

    function submitJoin() { root.ob.submitJoinInput(joinInput.text) }
    function submitSetup() { root.ob.setupWithCode(setupAddress.text, setupCode.text) }

    // ---- Join
    StartHead {
        step: root.screenLabel
        visible: !root.setup
        number: 1
        label: qsTr("Join a server")
        meta: qsTr("1 of 3")
        title: qsTr("Where do you want to go?")
    }
    ColumnLayout {
        objectName: "recentServers"
        visible: !root.setup && root.ob.recentServers.length > 0
        Layout.fillWidth: true
        spacing: 0
        UzSectionLabel { Layout.fillWidth: true; Layout.bottomMargin: Theme.space1; text: qsTr("Recent servers") }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
        Repeater {
            model: root.setup ? [] : root.ob.recentServers
            delegate: Item {
                id: recent
                required property var modelData
                objectName: "recentServer_" + modelData.hubId
                Layout.fillWidth: true
                implicitHeight: Theme.row44
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData.name + ", " + modelData.address
                Accessible.checked: modelData.selected
                Rectangle { anchors.fill: parent; color: recent.modelData.selected ? Theme.selected : (hover.hovered ? Theme.hover : "transparent") }
                Rectangle { visible: recent.modelData.selected; width: 2; height: parent.height; color: Theme.accent }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space15
                    anchors.rightMargin: Theme.space15
                    spacing: Theme.space2
                    Text {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: recent.modelData.name
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.body.size
                        font.weight: recent.modelData.selected ? Font.Medium : Font.Normal
                        color: Theme.ink
                    }
                    Text {
                        text: recent.modelData.address
                        textFormat: Text.PlainText
                        font.family: Theme.mono
                        font.pixelSize: Theme.roles.address.size
                        color: Theme.dim
                    }
                    Text {
                        visible: recent.modelData.lastUsed !== ""
                        text: qsTr("Last used %1").arg(root.player.whenText(recent.modelData.lastUsed))
                        textFormat: Text.PlainText
                        font.family: Theme.sans
                        font.pixelSize: Theme.roles.small.size
                        color: Theme.muted
                    }
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { recent.forceActiveFocus(); root.ob.selectRecentServer(recent.modelData.selected ? "" : recent.modelData.hubId) } }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                        root.ob.selectRecentServer(recent.modelData.selected ? "" : recent.modelData.hubId)
                        event.accepted = true
                    }
                }
                UzFocusRing { visible: recent.activeFocus; inset: true }
            }
        }
    }
    StartField {
        id: joinInput
        visible: !root.setup
        name: "joinInput"
        label: qsTr("Server address, invite link or invite code")
        placeholder: qsTr("server.example.com:8443 or an invite link")
        mono: true
        error: !root.setup ? root.ob.errorMessage : ""
        hint: qsTr("An invite code (UZ-8XK2-M4QD) does not say which server it is for: pick a recent server above, or Uzume asks for the address next.")
        onAccepted: root.submitJoin()
    }
    UzStatusIcon {
        objectName: "joinInviteKept"
        visible: !root.setup && root.ob.inviteCode !== ""
        kind: "done"
        small: true
        text: qsTr("Invite code %1 is kept for this server.").arg(root.ob.inviteCode)
    }
    RowLayout {
        visible: !root.setup
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            objectName: "joinContinue"
            id: joinContinueButton
            kind: "primary"
            busyOnClick: true
            enabled: joinInput.text.trim() !== "" || root.ob.selectedServerId !== ""
            Connections {
                target: root.ob
                function onChanged() { if (root.ob.errorCode !== "") Qt.callLater(() => joinContinueButton.clickBusy = false) }
            }
            text: qsTr("Continue")
            onClicked: root.submitJoin()
        }
        UzButton { objectName: "joinBack"; text: qsTr("Back"); onClicked: root.ob.back() }
        Item { Layout.fillWidth: true }
    }

    // ---- Set up: address and setup code (step 2)
    StartHead {
        step: root.screenLabel
        visible: root.setup
        number: 1
        label: qsTr("Set up a server")
        meta: qsTr("Step 2 of 4")
        title: qsTr("Connect to the device")
        text: qsTr("The server shows its address, a setup code and a security code. Enter the address and the setup code here.")
    }
    StartField {
        id: setupAddress
        visible: root.setup
        name: "setupAddress"
        label: qsTr("Address")
        placeholder: qsTr("192.0.2.10:8443")
        mono: true
        error: root.setup && !root.setupCodeError ? root.ob.errorMessage : ""
        onAccepted: setupCode.field.forceActiveFocus()
    }
    StartField {
        id: setupCode
        visible: root.setup
        name: "setupCode"
        label: qsTr("Setup code")
        placeholder: qsTr("7H4K-2Q")
        mono: true
        error: root.setup && root.setupCodeError ? root.ob.errorMessage : ""
        hint: qsTr("Or run `uzume-server setup-code` on the device.")
        onAccepted: root.submitSetup()
    }
    RowLayout {
        visible: root.setup
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            id: setupConnectButton
            objectName: "setupConnect"
            kind: "primary"
            busyOnClick: true
            enabled: setupAddress.text.trim() !== "" && setupCode.text.trim() !== ""
            Connections {
                target: root.ob
                function onChanged() { if (root.ob.errorCode !== "") Qt.callLater(() => setupConnectButton.clickBusy = false) }
            }
            text: qsTr("Connect")
            onClicked: root.submitSetup()
        }
        UzButton { objectName: "setupBack"; text: qsTr("Back"); onClicked: root.ob.back() }
        Item { Layout.fillWidth: true }
    }
}
