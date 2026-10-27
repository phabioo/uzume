import QtQuick
import QtQuick.Layouts
import Uzume.App

// 8b-4 / 8b-6 / 8b-5 Sign in on the server being joined, with every state of the form (signing in, wrong password, too many
// tries with the m:ss wait, offline and too old are their own pages).
StartPage {
    id: root
    required property PlayerController player
    readonly property var ob: player.onboarding
    signal forgotRequested()
    screenLabel: qsTr("Step 3 of 3 · %1").arg(ob.serverName)
    openLabel: qsTr("UZUME · SIGN IN · %1").arg(ob.serverName)
    initialFocus: form.nameField.field
    hints: [{ kbd: "Tab", label: qsTr("Next") }, { kbd: "Enter", label: qsTr("Sign in") },
            { pad: "A", label: qsTr("Choose") }, { pad: "Y", label: qsTr("Keyboard") }, { pad: "X", label: qsTr("Show password") },
            { kbd: "Esc", pad: "B", label: qsTr("Back") }]
    onBackRequested: ob.back()

    StartHead {
        step: root.screenLabel
        number: 1
        label: root.ob.serverName
        meta: qsTr("3 of 3")
        title: qsTr("Sign in")
    }
    StartSignInForm {
        id: form
        prefix: "signIn"
        busy: root.ob.busy
        errorCode: root.ob.errorCode
        errorMessage: root.ob.errorMessage
        retryAfter: root.ob.retryAfter
        onSignedIn: (name, password) => root.ob.signIn(name, password)
        onInviteRequested: root.ob.haveInvite()
        onForgotRequested: root.forgotRequested()
        onApprovalRequested: root.ob.requestApproval()
    }
}
