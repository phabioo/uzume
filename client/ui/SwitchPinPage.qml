import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8d-2 / 8d-3 PIN for this PC: the person, four square digits, the keypad and "Use password instead". A wrong PIN shows
// "Try n of 3" and how many tries are left; the third wrong PIN asks for the password (SwitchPasswordPage).
StartPage {
    id: root
    required property PlayerController player
    readonly property var ac: player.accounts
    readonly property bool wrong: ac.pinError === "wrong"
    screenLabel: qsTr("Switch user · PIN")
    openLabel: qsTr("UZUME · SWITCH USER · %1").arg(ac.targetName)
    maxWidth: 420
    initialFocus: pad
    hints: [{ kbd: "0-9", pad: "A", label: qsTr("Press") },
            { kbd: "Enter", label: qsTr("OK") },
            { kbd: "Backspace", pad: "B", label: qsTr("Delete") },
            { kbd: "Esc", pad: "Y", label: qsTr("Back to people") }]
    onBackRequested: ac.keepPlaying()

    function personIndex(id: string): int {
        let h = 0
        for (let i = 0; i < id.length; ++i) h = (h * 31 + id.charCodeAt(i)) % 4099
        return 1 + h % 4
    }

    StartHead {
        number: 1
        label: qsTr("PIN for this PC")
        meta: root.wrong ? qsTr("Try %1 of 3").arg(3 - root.ac.pinTriesLeft) : ""
        title: root.ac.targetName
        avatar: root.ac.targetName
        person: root.personIndex(root.ac.targetUserId)
        text: qsTr("Enter your PIN for this PC.")
    }
    PinPad {
        id: pad
        objectName: "pinPad"
        onSubmitted: (pin) => { root.ac.enterPin(pin); pad.clear() }
    }
    UzStatusIcon {
        objectName: "pinError"
        visible: root.wrong
        kind: "error"
        small: true
        text: root.ac.pinTriesLeft === 1 ? qsTr("Wrong PIN. 1 try left, then your password is needed.")
                                         : qsTr("Wrong PIN. %1 tries left, then your password is needed.").arg(root.ac.pinTriesLeft)
        maxWidth: 380
    }
    UzStatusIcon {
        objectName: "pinUnavailable"
        visible: root.ac.pinError === "unavailable"
        kind: "error"
        small: true
        text: qsTr("The PIN could not be checked because the system keyring is locked. Unlock it and try again, or use your password.")
        maxWidth: 380
    }
    UzButton {
        objectName: "pinUsePassword"
        kind: "link"
        size: "sm"
        text: qsTr("Use password instead")
        Layout.alignment: Qt.AlignLeft
        onClicked: root.ac.usePasswordInstead()
    }
    UzButton {
        objectName: "pinBack"
        kind: "ghost"
        size: "sm"
        text: qsTr("Back to people")
        Layout.alignment: Qt.AlignLeft
        onClicked: root.ac.keepPlaying()
    }
}
