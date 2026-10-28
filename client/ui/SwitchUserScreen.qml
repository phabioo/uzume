import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// Switch user (accounts-and-server.md 8d-1 to 8d-7): a full-window screen over every other screen, shown while
// AccountsController::phase is not "idle". Ctrl+Shift+U or "Switch user" in the person menu of the sidebar opens it
// (`player.accounts.open()`). One page per phase: people, quit the game first, PIN, password once, switching, add someone,
// remove from this PC. Esc is Back on every page.
FocusScope {
    id: root
    objectName: "switchUserScreen"
    required property PlayerController player
    readonly property var ac: player.accounts
    readonly property string phase: ac.phase
    readonly property Item currentPage: loader.item
    visible: phase !== "idle"
    focus: visible
    z: 60

    Rectangle {   // covers and blocks the screen below
        anchors.fill: parent
        color: Theme.ground
        MouseArea { anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }
    StartFrame {
        anchors.fill: parent
        screenLabel: root.currentPage ? root.currentPage.screenLabel : ""
        openLabel: root.currentPage ? root.currentPage.openLabel : "UZUME"
        openLines: ["GB · GBC · GBA · NDS · 3DS", qsTr("Version %1").arg(root.player.updates.shown(root.player.playerVersion)), root.player.deviceName]
        hints: root.currentPage ? root.currentPage.hints : []
        contentMaxWidth: root.currentPage ? root.currentPage.maxWidth : 520
        Loader {
            id: loader
            Layout.fillWidth: true
            active: root.visible
            sourceComponent: {
                switch (root.phase) {
                case "people": return peopleC
                case "quit_game": return quitC
                case "pin": return pinC
                case "password": return passwordC
                case "add": return addC
                case "confirm_remove": return removeC
                case "remove_blocked": return blockedC
                case "switching": return switchingC
                default: return null
                }
            }
            onLoaded: Qt.callLater(root.focusPage)
        }
    }
    function focusPage() {
        const f = loader.item ? loader.item.initialFocus : null
        if (f) f.forceActiveFocus()
        else root.forceActiveFocus()
    }
    onVisibleChanged: if (visible) Qt.callLater(root.focusPage)
    Keys.onEscapePressed: (event) => {
        if (loader.item) { loader.item.backRequested(); event.accepted = true }
    }

    Component { id: peopleC; SwitchPeoplePage { player: root.player } }
    Component { id: quitC; SwitchQuitPage { player: root.player } }
    Component { id: pinC; SwitchPinPage { player: root.player } }
    Component { id: passwordC; SwitchPasswordPage { player: root.player; onForgotRequested: forgot.open() } }
    Component { id: addC; SwitchAddPage { player: root.player; onForgotRequested: forgot.open() } }
    Component { id: removeC; SwitchRemovePage { player: root.player } }
    Component { id: blockedC; SwitchRemovePage { player: root.player; blocked: true } }
    Component {
        id: switchingC
        StartPage {
            screenLabel: qsTr("Switch user")
            openLabel: qsTr("UZUME · SWITCH USER")
            hints: [{ kbd: "Esc", pad: "B", label: qsTr("Cancel") }]
            onBackRequested: root.ac.close()
            StartHead { number: 1; label: qsTr("Switch user"); title: qsTr("Switching to %1").arg(root.ac.targetName) }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space15
                UzSpinner { objectName: "switchingSpinner"; size: 16 }
                Text { text: qsTr("Connecting as %1…").arg(root.ac.targetName); textFormat: Text.PlainText; font.family: Theme.sans; font.pixelSize: Theme.roles.body.size; color: Theme.ink2 }
            }
            UzStatusIcon { visible: root.ac.errorMessage !== ""; kind: "error"; small: true; text: root.ac.errorMessage; maxWidth: 480 }
            RowLayout {
                Layout.fillWidth: true
                UzButton { objectName: "switchingCancel"; text: qsTr("Cancel"); onClicked: root.ac.close() }
                Item { Layout.fillWidth: true }
            }
        }
    }

    ForgotPasswordDialog { id: forgot; objectName: "forgotDialogSwitch"; player: root.player }
}
