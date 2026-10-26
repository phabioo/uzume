import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Uzume.App

// The sign-in form of 8b-4 / 8b-6 / 8d-6 / 8d-7: Name, Password with "Forgot password?", Sign in, "I have an invite" and the
// line about asking an admin to approve this PC. The caller owns the request: it connects `signedIn(name, password)` and shows
// its state through `busy` and the error properties. Tab order: Name, Password, Sign in, I have an invite, Forgot password?.
ColumnLayout {
    id: root
    property string prefix: "signIn"           // objectName prefix of the controls
    property string nameText: ""
    property bool nameReadOnly: false
    property bool busy: false
    property string errorCode: ""
    property string errorMessage: ""
    property int retryAfter: 0
    property bool showInvite: true             // "I have an invite" / "Use an invite"
    property string inviteText: qsTr("I have an invite")
    property bool showApproval: true
    property string approvalLine: qsTr("No password yet? Sign in with your name only and an admin approves this PC.")
    readonly property alias nameField: nameInput
    readonly property alias passwordField: passwordInput
    signal signedIn(string name, string password)
    signal inviteRequested()
    signal forgotRequested()
    signal approvalRequested()
    spacing: Theme.space2
    Layout.fillWidth: true

    function submit() { root.signedIn(nameInput.text.trim(), passwordInput.text) }
    onNameTextChanged: nameInput.text = nameText

    StartField {
        id: nameInput
        name: root.prefix + "Name"
        label: qsTr("Name")
        text: root.nameText
        readOnly: root.nameReadOnly
        onAccepted: passwordInput.field.forceActiveFocus()
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        StartField {
            id: passwordInput
            name: root.prefix + "Password"
            label: qsTr("Password")
            secret: true
            error: root.errorCode === "invalid_credentials" ? root.errorMessage : ""
            onAccepted: root.submit()
        }
        UzButton {
            objectName: root.prefix + "Forgot"
            kind: "link"
            size: "sm"
            text: qsTr("Forgot password?")
            Layout.alignment: Qt.AlignLeft
            onClicked: root.forgotRequested()
        }
    }
    StartNotice {
        code: root.errorCode === "invalid_credentials" ? "" : root.errorCode
        message: root.errorMessage
        retryAfter: root.retryAfter
        who: nameInput.text.trim()
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space1
        UzButton {
            objectName: root.prefix + "Submit"
            kind: "primary"
            busy: root.busy
            enabled: !root.busy
            text: root.busy ? qsTr("Signing in…") : qsTr("Sign in")
            onClicked: root.submit()
        }
        UzButton {
            objectName: root.prefix + "Invite"
            visible: root.showInvite
            enabled: !root.busy
            text: root.inviteText
            onClicked: root.inviteRequested()
        }
        Item { Layout.fillWidth: true }
    }
    ColumnLayout {
        visible: root.showApproval
        Layout.fillWidth: true
        spacing: Theme.space05
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.rule2 }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.space2 - 1
            wrapMode: Text.Wrap
            text: root.approvalLine
            textFormat: Text.PlainText
            font.family: Theme.sans
            font.pixelSize: Theme.roles.small.size
            lineHeight: Theme.roles.small.line
            lineHeightMode: Text.FixedHeight
            color: Theme.muted
        }
        UzButton {
            objectName: root.prefix + "Approval"
            kind: "link"
            size: "sm"
            enabled: !root.busy
            text: qsTr("Ask an admin to approve this PC")
            onClicked: root.approvalRequested()
        }
    }
}
