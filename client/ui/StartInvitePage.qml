import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8b-3 Join with an invite: "Join <server>". The invite code comes from the link or from what was typed.
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    screenLabel: ob.flow === "link" ? qsTr("Invite link") : qsTr("Step 3 of 3 · %1").arg(ob.serverName)
    openLabel: qsTr("UZUME · JOIN A SERVER")
    initialFocus: form.codeText !== "" ? form.nameField.field : form.codeField.field
    hints: [{ kbd: "Tab", label: qsTr("Next") }, { kbd: "Enter", label: qsTr("Join") },
            { pad: "A", label: qsTr("Choose") }, { pad: "Y", label: qsTr("Keyboard") },
            { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: ob.back()

    StartHead {
        step: root.screenLabel
        number: 1
        label: root.ob.serverName
        meta: qsTr("3 of 3")
        title: qsTr("Join %1").arg(root.ob.serverName)
        text: qsTr("You were invited. Pick the name others will see.")
    }
    StartInviteForm {
        id: form
        prefix: "invite"
        serverName: root.ob.serverName
        codeText: root.ob.inviteCode
        busy: root.ob.busy
        errorCode: root.ob.errorCode
        errorMessage: root.ob.errorMessage
        retryAfter: root.ob.retryAfter
        onJoined: (code, name, password) => root.ob.redeemInvite(code, name, password)
        onHaveAccount: root.ob.haveAccount()
    }
}
