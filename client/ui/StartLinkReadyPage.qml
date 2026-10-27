import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8c-1 Invite link, first start: the linked server, its address and "Security code matches" (the link carries the server's
// code, so there is nothing to compare). Continue goes to the name step, Not now drops the link.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    screenLabel: qsTr("Invite link")
    openLabel: qsTr("UZUME · INVITE LINK")
    initialFocus: cont
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Continue") }, { kbd: "Esc", pad: "B", label: qsTr("Not now") }]
    onBackRequested: ob.cancel()

    StartHead {
        step: root.screenLabel
        number: 1
        label: qsTr("Invite link")
        title: qsTr("You're invited to %1").arg(root.ob.serverName)
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        Text {
            objectName: "linkServerAddress"
            text: root.ob.serverAddress
            textFormat: Text.PlainText
            font.family: Theme.mono
            font.pixelSize: Theme.roles.address.size
            color: Theme.dim
        }
        UzStatusIcon { objectName: "linkCodeMatches"; kind: "done"; text: qsTr("Security code matches") }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton { id: cont; objectName: "linkContinue"; kind: "primary"; text: qsTr("Continue"); onClicked: root.ob.continueLink() }
        UzButton { objectName: "linkNotNow"; text: qsTr("Not now"); onClicked: root.ob.cancel() }
        Item { Layout.fillWidth: true }
    }
}
