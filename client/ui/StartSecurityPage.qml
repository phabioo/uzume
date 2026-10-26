import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8b-2 Join / 8a-4 step 3 "Is this really <server>?": the security code of the server's certificate as a large Mono code and
// its address. It matches (primary, focused) remembers the certificate and goes on; It doesn't match is a hard stop and nothing
// is sent (OnboardingController::securityDiffers).
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    readonly property bool setup: ob.flow === "setup"
    screenLabel: setup ? qsTr("Set up a server · 3 of 4") : qsTr("Join a server · 2 of 3")
    openLabel: setup ? qsTr("UZUME · SET UP A SERVER") : qsTr("UZUME · JOIN A SERVER")
    initialFocus: matches
    hints: [{ kbd: "Enter", pad: "A", label: qsTr("Choose") }, { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: ob.back()

    StartHead {
        number: 1
        label: qsTr("Security check")
        meta: root.setup ? qsTr("Step 3 of 4") : qsTr("2 of 3")
        title: qsTr("Is this really %1?").arg(root.ob.serverName)
        text: root.setup ? qsTr("Compare this code with the security code shown next to the setup code on the device. If it matches, nobody is in between.")
                         : qsTr("Ask the person who runs it to read the code shown on the server. If it matches, nobody is in between.")
    }
    ColumnLayout {   // the code between two rules
        Layout.fillWidth: true
        spacing: 0
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 20
            Layout.bottomMargin: 20
            spacing: Theme.space05
            Text {
                objectName: "securityCode"
                Layout.fillWidth: true
                text: root.ob.securityCode
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.inviteCode.size
                font.weight: Theme.roles.inviteCode.weight
                font.letterSpacing: Theme.roles.inviteCode.spacing
                font.capitalization: Font.AllUppercase
                color: Theme.ink
                Accessible.name: qsTr("Security code %1").arg(root.ob.securityCode)
            }
            Text {
                objectName: "securityAddress"
                text: root.ob.serverAddress
                textFormat: Text.PlainText
                font.family: Theme.mono
                font.pixelSize: Theme.roles.address.size
                color: Theme.dim
            }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton { id: matches; objectName: "securityMatches"; kind: "primary"; text: qsTr("It matches"); onClicked: root.ob.securityMatches() }
        UzButton { objectName: "securityDiffers"; text: qsTr("It doesn’t match"); onClicked: root.ob.securityDiffers() }
        Item { Layout.fillWidth: true }
    }
    Text {
        objectName: "securityHelp"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.StyledText
        readonly property string command: "<font face='" + Theme.mono + "' color='" + Theme.ink2 + "'>uzume-server code</font>"
        text: root.setup ? qsTr("The device also shows it with %1.").arg(command)
                         : qsTr("The code is in Server › Settings › Network &amp; sharing, or on the server with %1.").arg(command)
        font.family: Theme.sans
        font.pixelSize: Theme.roles.small.size
        lineHeight: Theme.roles.small.line
        lineHeightMode: Text.FixedHeight
        color: Theme.muted
    }
}
