import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Settings > This PC > My account (accounts-and-server.md 8e-1 to 8e-5): the signed-in person with the display name (Rename…),
// the password (Set / Change / Remove…), the PIN for this PC (Set / Change / Remove) and My devices (Sign out, Sign out
// everywhere…). Left out until the server has the calls: "Ask for my password" and the Privacy toggles. The dialogs are shown
// over `host` (the Settings page).
ColumnLayout {
    id: root
    required property PlayerController player
    property Item host: null
    readonly property var ac: player.accounts
    readonly property var account: ac.account
    readonly property bool hasServer: player.hubAddress !== ""
    // The server of "Play on this PC" (child process): no account, password, PIN or devices (5a).
    readonly property bool pcOnly: player.localHub.onThisPcOnly
    readonly property bool loaded: (account.userId || "") !== ""
    readonly property bool canLoad: hasServer && ac.supported
    readonly property bool isAdmin: account.isAdmin === true
    readonly property string serverName: ac.serverName
    // Rows end where the controls of the SettingsRows end (the reset column stays empty on this page).
    readonly property real endMargin: width >= Theme.settingsNarrowBelow ? Theme.settingsResetWidth : 0
    spacing: Theme.space2

    function reload() {
        if (!root.canLoad) return
        root.ac.loadAccount()
        root.ac.loadDevices()
    }
    onCanLoadChanged: if (canLoad && visible) reload()
    onVisibleChanged: if (visible) reload()
    Component.onCompleted: if (visible) reload()
    Connections {
        target: root.player
        function onHubChanged() { if (root.visible) root.reload() }
    }
    Connections {
        target: root.ac
        function onDone(what: string) {
            if (what === "renamed") renameDialog.close()
            else if (what === "password_set") passwordDialog.close()
            else if (what === "password_removed") removePasswordDialog.close()
            else if (what === "pin_set") pinDialog.close()
            else if (what === "pin_removed") removePinDialog.close()
            else if (what === "signed_out_everywhere") signOutAllDialog.close()
        }
    }

    function openDialog(dialog) {
        root.ac.clearError()
        dialog.open()
    }
    function signInKindText(kind: string): string {
        switch (kind) {
        case "approval": return qsTr("Approved device")
        case "invite": return qsTr("Joined with an invite")
        case "password": return qsTr("Signed in with a password")
        default: return ""
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        Layout.rightMargin: root.endMargin > 0 ? root.endMargin - 8 : 0
        Eyebrow { objectName: "accountEyebrow"; Layout.fillWidth: true; text: qsTr("My account") }
    }

    // On this PC only (no server) or a server before accounts: one note, so only one wrapped label is ever laid out.
    UzLabel {
        objectName: !root.hasServer ? "accountNoServer" : "accountNotSupported"
        visible: !root.hasServer || !root.ac.supported
        Layout.fillWidth: true
        Layout.leftMargin: Theme.settingsDotSlot
        Layout.rightMargin: root.endMargin
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.roles.input.size
        color: Theme.muted
        text: !root.hasServer ? qsTr("No account needed on this PC. Join or set up a server to have one.")
                              : qsTr("This server is too old for accounts. Update the server first.")
    }

    // 5a On this PC only: "No account needed", the name for save files and screenshots, Share with others…
    ColumnLayout {
        objectName: "accountPcOnly"
        visible: root.pcOnly
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space2
        UzLabel {
            objectName: "accountPcOnlyNote"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.settingsDotSlot
            Layout.rightMargin: root.endMargin
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.small.size
            color: Theme.muted
            text: qsTr("No account needed. Your games and saves stay on this PC.")
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 0
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
            UzSettingsRow {
                objectName: "accountPcNameRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Your name")
                descriptionName: "accountPcNameValue"
                description: root.account.displayName || root.player.userName
                disabledReason: root.loaded ? "" : qsTr("Needs the server on this PC.")
                ctrl: "buttons"
                buttons: [{ name: "accountPcRename", text: qsTr("Rename…") }]
                onButtonClicked: { renameField.text = root.account.displayName || ""; root.openDialog(renameDialog); renameField.field.forceActiveFocus(); renameField.field.selectAll() }
            }
            UzSettingsRow {
                objectName: "accountPcShareRow"
                visible: root.player.localHub.shareSupported
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Share with others")
                description: qsTr("Run a server on this PC so friends can join. Your name gets a password.")
                ctrl: "buttons"
                buttons: [{ name: "accountPcShare", text: qsTr("Share with others…") }]
                onButtonClicked: root.player.localHub.openShareDialog("intro")
            }
            UzSettingsRow {
                objectName: "accountPcDiscordRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Show what I play on Discord")
                description: qsTr("Shows the game, the system and \"Session\" or the run name on your Discord profile while Discord runs on this PC. Off by default; never the server, other people or file paths.")
                ctrl: "toggle"
                toggleName: "accountPcDiscordToggle"
                checked: root.player.discordPresence
                onToggled: on => root.player.discordPresence = on
            }
        }
    }

    ColumnLayout {
        visible: root.hasServer && !root.pcOnly
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        spacing: Theme.space2

        // The person: avatar, name, role · server.
        RowLayout {
            objectName: "accountPerson"
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.leftMargin: Theme.settingsDotSlot
            spacing: Theme.space2
            UzAvatar {
                name: root.account.displayName || root.player.userName
                person: 1
                size: 40
                status: root.loaded ? "online" : ""
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 2
                Text {
                    objectName: "accountName"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    wrapMode: Text.Wrap
                    text: root.account.displayName || root.player.userName
                    textFormat: Text.PlainText
                    font.family: Theme.sans
                    font.pixelSize: Theme.roles.headerTitle.size
                    font.weight: Font.Medium
                    color: Theme.ink
                }
                Text {
                    objectName: "accountRole"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    elide: Text.ElideRight
                    visible: root.loaded
                    text: (root.isAdmin ? qsTr("Admin") : qsTr("Member")) + " · " + root.serverName
                    textFormat: Text.PlainText
                    font.family: Theme.mono
                    font.pixelSize: Theme.roles.meta.size
                    font.letterSpacing: Theme.roles.meta.spacing
                    font.capitalization: Font.AllUppercase
                    color: Theme.dim
                }
            }
        }
        UzLabel {
            objectName: "accountOffline"
            visible: !root.loaded
            Layout.fillWidth: true
            Layout.leftMargin: Theme.settingsDotSlot
            Layout.rightMargin: root.endMargin
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.input.size
            color: Theme.muted
            text: qsTr("Name, password and devices load when the server answers. The PIN for this PC works without it.")
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 0
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.rule2 }
            UzSettingsRow {
                objectName: "accountNameRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Display name")
                descriptionName: "accountNameValue"
                description: root.account.displayName || ""
                disabledReason: root.loaded ? "" : qsTr("Needs the server.")
                ctrl: "buttons"
                buttons: [{ name: "accountRename", text: qsTr("Rename…") }]
                onButtonClicked: { renameField.text = root.account.displayName || ""; root.openDialog(renameDialog); renameField.field.forceActiveFocus(); renameField.field.selectAll() }
            }
            UzSettingsRow {
                objectName: "accountPasswordRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Password")
                descriptionName: "accountPasswordValue"
                description: root.isAdmin ? (root.account.hasPassword ? qsTr("Required for admins.") : qsTr("Not set. Required for admins."))
                             : root.account.hasPassword ? qsTr("Set. Lets you sign in on another PC without waiting for an admin.")
                             : qsTr("Not set. Optional on %1. With a password you can sign in on another PC without waiting for an admin.").arg(root.serverName)
                disabledReason: root.loaded ? "" : qsTr("Needs the server.")
                ctrl: "buttons"
                buttons: root.account.hasPassword
                         ? [{ name: "accountPasswordChange", text: qsTr("Change…") },
                            { name: "accountPasswordRemove", text: qsTr("Remove…"), visible: !root.isAdmin && root.account.passwordRequired !== true }]
                         : [{ name: "accountPasswordSet", text: qsTr("Set password…") }]
                onButtonClicked: (name) => {
                    currentField.text = ""; newField.text = ""; againField.text = ""; removeCurrentField.text = ""
                    root.openDialog(name === "accountPasswordRemove" ? removePasswordDialog : passwordDialog)
                }
            }
            UzSettingsRow {
                objectName: "accountPinRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("PIN for this PC")
                descriptionName: "accountPinValue"
                description: root.account.hasPin ? qsTr("Set. Four digits, only on this PC; it never leaves it.")
                                                 : qsTr("Four digits to switch people faster on this PC. They work only here.")
                disabledReason: root.player.userName !== "" || root.loaded ? "" : qsTr("Needs a signed-in person.")
                ctrl: "buttons"
                buttons: root.account.hasPin
                         ? [{ name: "accountPinChange", text: qsTr("Change…") }, { name: "accountPinRemove", text: qsTr("Remove") }]
                         : [{ name: "accountPinSet", text: qsTr("Set PIN…") }]
                onButtonClicked: (name) => {
                    pinField.text = ""; pinAgainField.text = ""; pinDialog.localError = ""
                    if (name === "accountPinRemove") root.openDialog(removePinDialog)
                    else root.openDialog(pinDialog)
                }
            }
            UzSettingsRow {
                objectName: "accountDiscordRow"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                resetMode: "none"
                label: qsTr("Show what I play on Discord")
                description: qsTr("Shows the game, the system and \"Session\" or the run name on your Discord profile while Discord runs on this PC. Off by default; never the server, other people or file paths.")
                ctrl: "toggle"
                toggleName: "accountDiscordToggle"
                checked: root.player.discordPresence
                onToggled: on => root.player.discordPresence = on
            }
        }

        // My devices
        GridLayout {
            // The button moves below the eyebrow when the row is too narrow for both (wide fonts).
            id: devicesHead
            readonly property bool stackIt: devicesHead.width < devicesEyebrow.implicitWidth + signOutAll.implicitWidth + Theme.space2
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.topMargin: Theme.space2
            Layout.rightMargin: root.endMargin > 0 ? root.endMargin - 8 : 0
            columns: devicesHead.stackIt ? 1 : 2
            columnSpacing: Theme.space2
            rowSpacing: Theme.space1
            Eyebrow { id: devicesEyebrow; objectName: "accountDevicesEyebrow"; Layout.fillWidth: true; text: qsTr("My devices · %1").arg(root.ac.devices.length) }
            UzButton {
                id: signOutAll
                objectName: "accountSignOutEverywhere"
                kind: "danger"
                size: "sm"
                Layout.minimumWidth: 0
                Layout.fillWidth: devicesHead.stackIt   // a button wider than the page elides its label instead of widening the page
                enabled: root.loaded
                text: qsTr("Sign out everywhere…")
                onClicked: root.openDialog(signOutAllDialog)
            }
        }
        Repeater {
            model: root.ac.devices
            delegate: Item {
                id: deviceRow
                required property var modelData
                objectName: "accountDevice_" + modelData.deviceId
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.leftMargin: Theme.settingsDotSlot
                Layout.rightMargin: root.endMargin
                implicitHeight: Math.max(rowLayout.implicitHeight + 24, Theme.row44)
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.rule2 }
                RowLayout {
                    id: rowLayout
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space2
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        spacing: 2
                        RowLayout {
                            spacing: Theme.space1
                            Text {
                                text: deviceRow.modelData.name
                                textFormat: Text.PlainText
                                font.family: Theme.sans
                                font.pixelSize: Theme.roles.input.size
                                font.weight: Font.Medium
                                color: Theme.ink
                            }
                            UzTag { visible: deviceRow.modelData.current === true; text: qsTr("This device") }
                            UzTag { visible: deviceRow.modelData.revoked === true; text: qsTr("Signed out") }
                        }
                        UzMono {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: Theme.roles.meta11.size
                            color: Theme.dim
                            text: [deviceRow.modelData.platform, deviceRow.modelData.appVersion,
                                   deviceRow.modelData.lastSeen !== "" ? qsTr("Last seen %1").arg(root.player.whenText(deviceRow.modelData.lastSeen)) : "",
                                   root.signInKindText(deviceRow.modelData.signInKind)].filter(t => t !== "").join(" · ")
                        }
                    }
                    UzButton {
                        objectName: "accountDeviceSignOut_" + deviceRow.modelData.deviceId
                        visible: deviceRow.modelData.revoked !== true
                        implicitHeight: 32
                        hPadding: 24
                        text: qsTr("Sign out")
                        onClicked: root.ac.signOutDevice(deviceRow.modelData.deviceId)
                    }
                }
            }
        }
        UzLabel {
            objectName: "accountNoDevices"
            visible: root.ac.devices.length === 0
            Layout.fillWidth: true
            Layout.leftMargin: Theme.settingsDotSlot
            Layout.rightMargin: root.endMargin
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.roles.input.size
            color: Theme.muted
            text: qsTr("Your devices appear here when the server answers.")
        }
    }

    // ---- Dialogs (over the Settings page)
    UzDialog {
        id: renameDialog
        objectName: "renameDialog"
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: qsTr("Rename")
        text: qsTr("The name others see, also on your saves and in Sessions.")
        error: root.ac.errorMessage
        busy: root.ac.busy
        StartField { fieldFill: Theme.ground; id: renameField; name: "renameField"; label: qsTr("Display name"); onAccepted: renameSave.clicked() }
        actions: [
            UzButton { id: renameSave; objectName: "renameSave"; kind: "primary"; text: qsTr("Save"); enabled: renameField.text.trim() !== ""; onClicked: root.ac.rename(renameField.text) },
            UzButton { objectName: "renameCancel"; text: qsTr("Cancel"); onClicked: renameDialog.close() }
        ]
    }
    UzDialog {
        id: passwordDialog
        objectName: "passwordDialog"
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: root.account.hasPassword ? qsTr("Change password") : qsTr("Set password")
        text: qsTr("Your other devices stay signed in. To sign them out, use Sign out everywhere.")
        error: root.ac.errorCode === "password_too_short" ? "" : root.ac.errorMessage
        busy: root.ac.busy
        StartField { fieldFill: Theme.ground; id: currentField; name: "passwordCurrent"; visible: root.account.hasPassword === true; label: qsTr("Current password"); secret: true; onAccepted: newField.field.forceActiveFocus() }
        StartField { fieldFill: Theme.ground; id: newField; name: "passwordNew"; label: qsTr("New password"); secret: true
            error: root.ac.errorCode === "password_too_short" ? qsTr("At least 12 characters.") : ""
            onAccepted: againField.field.forceActiveFocus() }
        StartField { fieldFill: Theme.ground; id: againField; name: "passwordAgain"; label: qsTr("New password again"); secret: true; onAccepted: passwordSave.clicked() }
        actions: [
            UzButton { id: passwordSave; objectName: "passwordSave"; kind: "primary"; text: qsTr("Save"); onClicked: root.ac.setPassword(currentField.text, newField.text, againField.text) },
            UzButton { objectName: "passwordCancel"; text: qsTr("Cancel"); onClicked: passwordDialog.close() }
        ]
    }
    UzDialog {
        id: removePasswordDialog
        objectName: "removePasswordDialog"
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: qsTr("Remove your password?")
        text: qsTr("Without a password an admin has to approve each new PC you sign in on. You can set one again at any time.")
        error: root.ac.errorMessage
        busy: root.ac.busy
        StartField { fieldFill: Theme.ground; id: removeCurrentField; name: "passwordRemoveCurrent"; label: qsTr("Current password"); secret: true; onAccepted: removePasswordConfirm.clicked() }
        actions: [
            UzButton { objectName: "passwordRemoveCancel"; kind: "primary"; text: qsTr("Cancel"); onClicked: removePasswordDialog.close() }
        ]
        dangerActions: [
            UzButton { id: removePasswordConfirm; objectName: "passwordRemoveConfirm"; kind: "danger"; text: qsTr("Remove password"); onClicked: root.ac.removePassword(removeCurrentField.text) }
        ]
    }
    UzDialog {
        id: pinDialog
        objectName: "pinDialog"
        property string localError: ""
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: root.account.hasPin ? qsTr("Change PIN") : qsTr("Set a PIN for this PC")
        text: qsTr("Four digits. The PIN works only on this PC, it never leaves it.")
        error: localError !== "" ? localError : root.ac.errorMessage
        StartField { fieldFill: Theme.ground; id: pinField; name: "pinNew"; label: qsTr("PIN"); secret: true; onAccepted: pinAgainField.field.forceActiveFocus() }
        StartField { fieldFill: Theme.ground; id: pinAgainField; name: "pinAgain"; label: qsTr("PIN again"); secret: true; onAccepted: pinSave.clicked() }
        actions: [
            UzButton {
                id: pinSave
                objectName: "pinSave"
                kind: "primary"
                text: qsTr("Save")
                onClicked: {
                    if (pinField.text !== pinAgainField.text) { pinDialog.localError = qsTr("The two PINs are not the same."); return }
                    pinDialog.localError = ""
                    root.ac.setPin(pinField.text)
                }
            },
            UzButton { objectName: "pinCancel"; text: qsTr("Cancel"); onClicked: pinDialog.close() }
        ]
    }
    UzDialog {
        id: removePinDialog
        objectName: "removePinDialog"
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: qsTr("Remove the PIN?")
        text: qsTr("Switching to you on this PC asks for your password until you set a new PIN.")
        actions: [
            UzButton { objectName: "pinRemoveCancel"; kind: "primary"; text: qsTr("Cancel"); onClicked: removePinDialog.close() }
        ]
        dangerActions: [
            UzButton { objectName: "pinRemoveConfirm"; kind: "danger"; text: qsTr("Remove PIN"); onClicked: root.ac.removePin() }
        ]
    }
    UzDialog {
        id: signOutAllDialog
        objectName: "signOutAllDialog"
        property bool includeThis: false
        parent: root.host ? root.host : root
        label: qsTr("My account")
        title: qsTr("Sign out on all devices?")
        text: qsTr("Every device needs your password again. Saves that are uploaded are safe.")
        error: root.ac.errorMessage
        busy: root.ac.busy
        UzStatusIcon {
            kind: "warning"
            small: true
            maxWidth: 340
            text: qsTr("A device that holds a save which isn't uploaded yet uploads it after the next sign-in there. It is not lost.")
        }
        UzToggle {
            objectName: "signOutIncludeThis"
            text: qsTr("Also sign out this device")
            checked: signOutAllDialog.includeThis
            onToggled: signOutAllDialog.includeThis = checked
        }
        actions: [
            UzButton { objectName: "signOutAllCancel"; kind: "primary"; text: qsTr("Cancel"); onClicked: signOutAllDialog.close() }
        ]
        dangerActions: [
            UzButton { objectName: "signOutAllConfirm"; kind: "danger"; text: qsTr("Sign out everywhere"); onClicked: root.ac.signOutEverywhere(signOutAllDialog.includeThis) }
        ]
    }
}
